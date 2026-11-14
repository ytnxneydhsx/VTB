use std::collections::HashMap;
use std::sync::Arc;
use std::time::Instant;

use ayagami::meta::DisplayInfo;
use godot::classes::file_access::ModeFlags;
use godot::classes::notify::CanvasItemNotification;
use godot::classes::{ArrayMesh, FileAccess, INode2D, MeshInstance2D, ShaderMaterial, SubViewport};
use godot::meta::ClassId;
use godot::prelude::*;

use ayagami::core::{Item, Model, Param};
use ayagami::driver::{DrawNode, Driver};
use ayagami::file::ParsedModel;
use ayagami::pose::{Key, Pose, PoseMap, Value};
use godot::register::info::{PropertyHint, PropertyHintInfo, PropertyInfo, PropertyUsageFlags};

use crate::mutator::IMutator;

pub const PARAMETER_PREFIX: &str = "parameters/";
const PARAMETER_RANGE_SUFFIX: &str = "/range";
const PARAMETER_DEFAULT_SUFFIX: &str = "/default";
const PARAMETER_GROUP_SUFFIX: &str = "/group";
const OUTPUT_PREFIX: &str = "output/";

pub const PART_PREFIX: &str = "parts/";

pub fn key_param<'a>(property: StringName) -> Key<'a> {
    Key::from_param(property.trim_prefix(PARAMETER_PREFIX).to_string())
}
pub fn key_part<'a>(property: StringName) -> Key<'a> {
    Key::from_part(property.trim_prefix(PART_PREFIX).to_string())
}
pub fn param_to_key<'a>(property: StringName) -> Option<Key<'a>> {
    if property.begins_with(PARAMETER_PREFIX) {
        Some(key_param(property))
    } else if property.begins_with(PART_PREFIX) {
        Some(key_part(property))
    } else {
        None
    }
}

pub struct LoadedModel<T: Model, R: AsRef<T>> {
    pub model: R,
    pub driver: Driver<T>,
}

fn env_flag(name: &str, default: bool) -> bool {
    let Ok(value) = std::env::var(name) else {
        return default;
    };

    match value.to_ascii_lowercase().as_str() {
        "0" | "false" | "no" | "off" => false,
        "1" | "true" | "yes" | "on" => true,
        _ => default,
    }
}

struct MeshRenderCache {
    instance: Gd<MeshInstance2D>,
    mesh: Gd<ArrayMesh>,
    use_driver_colors: bool,
    last_visible: Option<bool>,
    last_opacity: Option<f32>,
    last_multiply: Option<Color>,
    last_screen: Option<Color>,
    vertices: PackedVector2Array,
}

struct MaskRenderCache {
    viewport: Gd<SubViewport>,
    source_meshes: Vec<Gd<ArrayMesh>>,
    dependent_meshes: Vec<Gd<MeshInstance2D>>,
}

#[derive(Default)]
struct FrameProfile {
    frames: u64,
    mesh_candidates: u64,
    updated_meshes: u64,
    updated_vertices: u64,
    uploaded_vertex_bytes: u64,
    drive_ns: u128,
    mesh_submit_ns: u128,
    vertex_convert_ns: u128,
    to_byte_array_ns: u128,
    vertex_upload_call_ns: u128,
    masks_ns: u128,
    reorder_ns: u128,
}

#[derive(Default)]
struct MeshSubmitProfile {
    updated_meshes: u64,
    updated_vertices: u64,
    uploaded_vertex_bytes: u64,
    vertex_convert_ns: u128,
    to_byte_array_ns: u128,
    vertex_upload_call_ns: u128,
}

#[derive(GodotClass)]
#[class(tool, init, base = Node2D)]
pub struct AyagamiModel {
    base: Base<Node2D>,
    #[var(pub)]
    size: Vector2i,
    #[var(pub)]
    origin: Vector2,
    #[export(range = (128.0, 8096.0))]
    #[init(val = 2048)]
    mask_resolution: i32,

    pub model: Option<LoadedModel<ParsedModel, Box<ParsedModel>>>,
    display_info: Option<DisplayInfo>,
    parameter_groups: HashMap<u32, GString>,

    dirty: bool,

    mesh_group: Option<Gd<Node2D>>,
    meshes: HashMap<u32, MeshRenderCache>,
    all_mesh_uids: Vec<u32>,
    updated_mesh_uids: Vec<u32>,
    masks: Vec<MaskRenderCache>,
    state_cache_enabled: bool,
    driver_buffer_reuse_enabled: bool,
    updated_mesh_list_enabled: bool,
    profile_enabled: bool,
    frame_profile: FrameProfile,
    last_mesh_candidates: u64,
    mask_layout_dirty: bool,
    last_mask_base_scale: Option<f32>,
    last_mask_resolution: i32,

    pub pose: Option<Pose>,
    pub pose_map: Arc<PoseMap>,
    pub last_pose: Option<Pose>,
}

#[godot_api]
impl AyagamiModel {
    #[signal]
    pub fn loaded();

    #[func]
    fn is_loaded(&self) -> bool {
        self.model.is_some()
    }

    pub fn load(&mut self) {
        let resource_path = &self.base().get_scene_file_path();

        let base_path = resource_path.get_base_dir();
        let settings: ayagami::meta::Model3 = {
            godot_print!("model path: {}", resource_path);
            let json = FileAccess::get_file_as_string(resource_path);
            serde_json::from_str(&json.to_string()).expect("unable to parse model3 json")
        };

        let model = {
            let file_path = base_path.path_join(&settings.file_references.moc.to_gstring());
            let mut f = GFile::open(&file_path, ModeFlags::READ).unwrap();
            Box::new(ParsedModel::load(&mut f).unwrap())
        };
        self.display_info = if let Some(path) = settings.file_references.display_info {
            let file_path = base_path.path_join(&path);
            let json = FileAccess::get_file_as_string(&file_path);
            serde_json::from_str(&json.to_string()).ok()
        } else {
            None
        };

        if let Some(di) = self.display_info.as_ref() {
            for group in di.parameter_groups.clone() {
                for param in di.parameters.clone() {
                    if param.group_id == group.id {
                        if let Some(p) = model.params().into_iter().find(|p| p.id() == param.id) {
                            self.parameter_groups
                                .insert(p.uid(), group.name.to_gstring());
                        }
                    }
                }
            }
        }

        self.pose_map = model.pose_map().clone();
        self.pose = {
            let mut pose = Pose::new(&*model);
            pose.flatten();
            Some(pose)
        };
        self.state_cache_enabled = env_flag("OPENVT_AYAGAMI_STATE_CACHE", true);
        self.driver_buffer_reuse_enabled =
            env_flag("OPENVT_AYAGAMI_DRIVER_BUFFER_REUSE", true);
        self.updated_mesh_list_enabled = env_flag("OPENVT_AYAGAMI_UPDATED_MESH_LIST", true);
        self.profile_enabled = env_flag("OPENVT_AYAGAMI_PROFILE", false);

        let mut driver = Driver::new(&*model.as_ref());
        driver.set_buffer_reuse_enabled(self.driver_buffer_reuse_enabled);

        godot_print!(
            "AYAGAMI_OPTIMIZATION state_cache={} driver_buffer_reuse={} updated_mesh_list={} profile={}",
            self.state_cache_enabled,
            self.driver_buffer_reuse_enabled,
            self.updated_mesh_list_enabled,
            self.profile_enabled,
        );

        let loaded = LoadedModel { model, driver };
        self.model = Some(loaded);

        self.dirty = true;
    }

    fn reorder_meshes(&mut self) {
        let mesh_group = self.mesh_group.as_mut().unwrap();
        let md = self.model.as_mut().unwrap();

        // reorder meshes if dirty to properly maintain z-index
        // if Godot ever implements sorting groups (https://github.com/godotengine/godot-proposals/issues/9428)
        // then we will be able to sensibly use z-index
        // but as long as z-index is a global sort order, it's better for use the scene tree
        // and pray that a model isn't constantly changing its render order
        if md.driver.order_changed() {
            let mut order = 0;
            for node in md.driver.draw_nodes(None).unwrap() {
                let DrawNode::ArtMesh(uid) = node else {
                    continue;
                };
                let mesh_instance = &self.meshes[uid].instance;

                mesh_group.move_child(mesh_instance, order as i32);
                order += 1;
            }
        }
    }

    fn drive_model(&mut self) {
        let md = self.model.as_mut().unwrap();
        md.driver.drive(md.model.as_ref());
        self.updated_mesh_uids.clear();
        self.updated_mesh_uids
            .extend_from_slice(md.driver.updated_artmeshes());
    }

    fn record_frame_profile(
        &mut self,
        drive_ns: u128,
        mesh_submit_ns: u128,
        mesh_profile: MeshSubmitProfile,
        masks_ns: u128,
        reorder_ns: u128,
    ) {
        const REPORT_INTERVAL: u64 = 600;

        self.frame_profile.frames += 1;
        self.frame_profile.mesh_candidates += self.last_mesh_candidates;
        self.frame_profile.updated_meshes += mesh_profile.updated_meshes;
        self.frame_profile.updated_vertices += mesh_profile.updated_vertices;
        self.frame_profile.uploaded_vertex_bytes += mesh_profile.uploaded_vertex_bytes;
        self.frame_profile.drive_ns += drive_ns;
        self.frame_profile.mesh_submit_ns += mesh_submit_ns;
        self.frame_profile.vertex_convert_ns += mesh_profile.vertex_convert_ns;
        self.frame_profile.to_byte_array_ns += mesh_profile.to_byte_array_ns;
        self.frame_profile.vertex_upload_call_ns += mesh_profile.vertex_upload_call_ns;
        self.frame_profile.masks_ns += masks_ns;
        self.frame_profile.reorder_ns += reorder_ns;

        if self.frame_profile.frames < REPORT_INTERVAL {
            return;
        }

        self.print_frame_profile();
        self.frame_profile = FrameProfile::default();
    }

    fn print_frame_profile(&self) {
        if self.frame_profile.frames == 0 {
            return;
        }

        let frames = self.frame_profile.frames as f64;
        let ns_to_ms = |ns: u128| ns as f64 / frames / 1_000_000.0;
        let pipeline_ns = self.frame_profile.drive_ns
            + self.frame_profile.mesh_submit_ns
            + self.frame_profile.masks_ns
            + self.frame_profile.reorder_ns;
        godot_print!(
            "AYAGAMI_PROFILE_WORK frames={} mesh_candidates_avg={:.3} updated_meshes_avg={:.3} updated_vertices_avg={:.3} vertex_bytes_avg={:.3}",
            self.frame_profile.frames,
            self.frame_profile.mesh_candidates as f64 / frames,
            self.frame_profile.updated_meshes as f64 / frames,
            self.frame_profile.updated_vertices as f64 / frames,
            self.frame_profile.uploaded_vertex_bytes as f64 / frames,
        );
        godot_print!(
            "AYAGAMI_PROFILE_VERTEX frames={} convert_ms={:.6} to_byte_array_ms={:.6} upload_call_ms={:.6} mesh_submit_ms={:.6}",
            self.frame_profile.frames,
            ns_to_ms(self.frame_profile.vertex_convert_ns),
            ns_to_ms(self.frame_profile.to_byte_array_ns),
            ns_to_ms(self.frame_profile.vertex_upload_call_ns),
            ns_to_ms(self.frame_profile.mesh_submit_ns),
        );
        godot_print!(
            "AYAGAMI_PROFILE_PIPELINE frames={} drive_ms={:.6} masks_ms={:.6} reorder_ms={:.6} pipeline_ms={:.6}",
            self.frame_profile.frames,
            ns_to_ms(self.frame_profile.drive_ns),
            ns_to_ms(self.frame_profile.masks_ns),
            ns_to_ms(self.frame_profile.reorder_ns),
            ns_to_ms(pipeline_ns),
        );
    }

    #[func]
    fn profile_reset(&mut self) {
        self.frame_profile = FrameProfile::default();
    }

    #[func]
    fn profile_report(&self) {
        self.print_frame_profile();
    }

    fn update_meshes(&mut self, force: bool) -> MeshSubmitProfile {
        let mut profile = MeshSubmitProfile::default();
        let state_cache_enabled = self.state_cache_enabled;
        let mut mask_layout_changed = force;
        let use_updated_mesh_list = self.updated_mesh_list_enabled && !force;
        self.last_mesh_candidates = if use_updated_mesh_list {
            self.updated_mesh_uids.len() as u64
        } else {
            self.all_mesh_uids.len() as u64
        };
        let selected_uids = if use_updated_mesh_list {
            &self.updated_mesh_uids
        } else {
            &self.all_mesh_uids
        };
        let md = self.model.as_mut().unwrap();

        let px_size = md.model.canvas_properties().scale;

        // update mesh vertices and submit visual state only when it changed
        for uid in selected_uids.iter().copied() {
            let cache = self.meshes.get_mut(&uid).unwrap();
            let mut mesh = if state_cache_enabled {
                cache.mesh.clone()
            } else {
                cache.instance.get_mesh().unwrap().cast::<ArrayMesh>()
            };
            let mesh_instance = &mut cache.instance;
            let m = md.driver.artmesh_state(uid).unwrap();
            let verts = m.vertices;
            let count = verts.len();

            if count < 3 {
                if !state_cache_enabled || cache.last_visible != Some(false) {
                    mesh_instance.set_visible(false);
                }
                cache.last_visible = Some(false);
                continue;
            }

            if !state_cache_enabled || cache.last_visible != Some(m.visual.visible) {
                mesh_instance.set_visible(m.visual.visible);
            }
            cache.last_visible = Some(m.visual.visible);

            if !state_cache_enabled || cache.last_opacity != Some(m.visual.opacity) {
                mesh_instance.set_self_modulate(Color {
                    r: 1.0,
                    g: 1.0,
                    b: 1.0,
                    a: m.visual.opacity,
                });
            }
            cache.last_opacity = Some(m.visual.opacity);

            let use_driver_colors = if state_cache_enabled {
                cache.use_driver_colors
            } else {
                mesh_instance.get_instance_shader_parameter("color_override") != true.to_variant()
            };
            if use_driver_colors {
                let multiply = Color {
                    r: m.visual.multiply_color.x,
                    g: m.visual.multiply_color.y,
                    b: m.visual.multiply_color.z,
                    a: 1.0,
                };
                let screen = Color {
                    r: m.visual.screen_color.x,
                    g: m.visual.screen_color.y,
                    b: m.visual.screen_color.z,
                    a: 1.0,
                };
                if !state_cache_enabled || cache.last_multiply != Some(multiply) {
                    mesh_instance.set_instance_shader_parameter(
                        "color_multiply",
                        &multiply.to_variant(),
                    );
                }
                if !state_cache_enabled || cache.last_screen != Some(screen) {
                    mesh_instance
                        .set_instance_shader_parameter("color_screen", &screen.to_variant());
                }
                cache.last_multiply = Some(multiply);
                cache.last_screen = Some(screen);
            }

            if !force {
                if !m.updated {
                    continue;
                }

                if !m.visual.visible {
                    continue;
                }
            }

            let mut vertices = if state_cache_enabled {
                std::mem::replace(&mut cache.vertices, PackedVector2Array::new())
            } else {
                PackedVector2Array::new()
            };
            vertices.resize(count);

            profile.updated_meshes += 1;
            profile.updated_vertices += count as u64;
            profile.uploaded_vertex_bytes += (count * std::mem::size_of::<Vector2>()) as u64;

            let mut vtx_min = Vector3::new(f32::MAX, f32::MAX, 0.0); // top-left
            let mut vtx_max = Vector3::new(f32::MIN, f32::MIN, 0.0); // bottom-right

            let vertex_convert_start = self.profile_enabled.then(Instant::now);
            for (i, vtx) in verts.iter().enumerate() {
                let x = vtx.x * px_size;
                let y = vtx.y * px_size;
                vtx_min.x = f32::min(vtx_min.x, x); // left
                vtx_min.y = f32::min(vtx_min.y, y); // top
                vtx_max.x = f32::max(vtx_max.x, x); // right
                vtx_max.y = f32::max(vtx_max.y, y); // bottom

                vertices[i] = Vector2::new(x, y);
            }
            profile.vertex_convert_ns += vertex_convert_start
                .map(|start| start.elapsed().as_nanos())
                .unwrap_or(0);

            let to_byte_array_start = self.profile_enabled.then(Instant::now);
            let vertex_bytes = vertices.to_byte_array();
            profile.to_byte_array_ns += to_byte_array_start
                .map(|start| start.elapsed().as_nanos())
                .unwrap_or(0);

            let vertex_upload_call_start = self.profile_enabled.then(Instant::now);
            mesh.surface_update_vertex_region(0, 0, &vertex_bytes);
            profile.vertex_upload_call_ns += vertex_upload_call_start
                .map(|start| start.elapsed().as_nanos())
                .unwrap_or(0);
            if state_cache_enabled {
                cache.vertices = vertices;
            }

            // aabb does not get automatically updated when directly updating the vertex region
            let aabb = Aabb::new(Vector3::new(vtx_min.x, vtx_min.y, 0.0), vtx_max - vtx_min);
            let existing_aabb = mesh.get_custom_aabb();

            // only mark as dirty if the bounds of the mesh have shifted so that we may
            // update affected mask viewports
            if aabb != existing_aabb {
                mesh.set_custom_aabb(aabb);
                mask_layout_changed = true;
            }
        }

        if mask_layout_changed {
            self.mask_layout_dirty = true;
        }
        profile
    }

    fn update_masks(&mut self) {
        let base_scale = self.base().get_global_scale().x.min(1.0);
        let mask_inputs_changed = self.last_mask_base_scale != Some(base_scale)
            || self.last_mask_resolution != self.mask_resolution;

        if self.state_cache_enabled && !self.mask_layout_dirty && !mask_inputs_changed {
            return;
        }

        // update viewport dimensions and transform for masks
        for mask in self.masks.iter_mut() {
            if mask.source_meshes.is_empty() {
                continue;
            }

            let mut group_aabb = mask.source_meshes[0].get_custom_aabb();
            for mesh in mask.source_meshes.iter().skip(1) {
                group_aabb = group_aabb.merge(mesh.get_custom_aabb());
            }

            group_aabb = group_aabb.grow(4.0);

            let mut dimensions = Vector2 {
                x: group_aabb.size.x as f32,
                y: group_aabb.size.y as f32,
            };
            let offset = Vector2 {
                x: group_aabb.position.x,
                y: group_aabb.position.y,
            };

            let resolution = self.mask_resolution as f32;
            let scale: f32 = if dimensions.x > resolution || dimensions.y > resolution {
                resolution / dimensions.x.max(dimensions.y)
            } else {
                1.0
            } * base_scale;
            dimensions *= scale;

            let transform =
                Transform2D::from_angle_origin(0.0, -offset).scaled(Vector2::ONE * scale);

            mask.viewport.set_size(dimensions.to_vector2i());
            mask.viewport.set_canvas_transform(transform);

            for node in mask.dependent_meshes.iter_mut() {
                node.set_instance_shader_parameter("mask_offset", &offset.to_variant());
                node.set_instance_shader_parameter("mask_scale", &scale.to_variant());
            }
        }

        self.mask_layout_dirty = false;
        self.last_mask_base_scale = Some(base_scale);
        self.last_mask_resolution = self.mask_resolution;
    }

    #[func]
    pub fn invalidate_render_state_cache(&mut self) {
        for cache in self.meshes.values_mut() {
            cache.use_driver_colors =
                cache.instance.get_instance_shader_parameter("color_override") != true.to_variant();
            cache.last_visible = None;
            cache.last_opacity = None;
            cache.last_multiply = None;
            cache.last_screen = None;
        }
        self.mask_layout_dirty = true;
        self.last_mask_base_scale = None;
    }

    #[func]
    pub fn get_parameters(&self) -> Array<StringName> {
        let mut arr: Vec<_> = self
            .pose_map
            .keys()
            .filter_map(|k| match k {
                Key::Param(id) => Some(id.to_string()),
                _ => None,
            })
            .collect();
        arr.sort();

        Array::from_iter(arr.iter().map(|v| v.to_string_name()))
    }

    #[func]
    pub fn get_parts(&self) -> Array<StringName> {
        let mut arr: Vec<_> = self
            .pose_map
            .keys()
            .filter_map(|k| match k {
                Key::Part(id) => Some(id.to_string()),
                _ => None,
            })
            .collect();
        arr.sort();

        Array::from_iter(arr.iter().map(|v| v.to_string_name()))
    }
}

#[godot_api]
impl INode2D for AyagamiModel {
    fn on_notification(&mut self, what: CanvasItemNotification) {
        // reconnect scene to an ayagami driver when instantiated from an imported resource
        if what == CanvasItemNotification::READY {
            if !self.is_loaded() {
                self.load();
            }

            // cache references so we don't have to go through the scene tree every update
            let mesh_group = self.base().get_node_as::<Node2D>("Meshes");
            self.mesh_group = Some(mesh_group.clone());
            self.meshes =
                mesh_group
                    .get_children()
                    .iter_shared()
                    .fold(HashMap::new(), |mut acc, n| {
                        let uid = n.get_meta("uid").to::<u32>();
                        let instance = n.cast::<MeshInstance2D>();
                        let mesh = instance.get_mesh().unwrap().cast::<ArrayMesh>();
                        let use_driver_colors = instance
                            .get_instance_shader_parameter("color_override")
                            != true.to_variant();
                        acc.insert(
                            uid,
                            MeshRenderCache {
                                instance,
                                mesh,
                                use_driver_colors,
                                last_visible: None,
                                last_opacity: None,
                                last_multiply: None,
                                last_screen: None,
                                vertices: PackedVector2Array::new(),
                            },
                        );
                        acc
                    });

            let mask_group = self.base().get_node_as::<Node>("Masks");
            self.masks = mask_group
                .get_children()
                .iter_shared()
                .map(|n| {
                    let viewport = n.cast::<SubViewport>();
                    let source_meshes = viewport
                        .get_children()
                        .iter_shared()
                        .map(|child| child.cast::<MeshInstance2D>())
                        .map(|instance| instance.get_mesh().unwrap().cast::<ArrayMesh>())
                        .collect();
                    let dependent_meshes = viewport
                        .get_meta("meshes")
                        .to::<VarArray>()
                        .iter_shared()
                        .map(|np| np.to::<NodePath>())
                        .map(|path| self.base().get_node_as::<MeshInstance2D>(&path))
                        .collect();
                    MaskRenderCache {
                        viewport,
                        source_meshes,
                        dependent_meshes,
                    }
                })
                .collect();
            self.all_mesh_uids = self.meshes.keys().copied().collect();
            self.mask_layout_dirty = true;
            self.last_mask_base_scale = None;

            self.drive_model();
            self.update_meshes(true);
            self.update_masks();
            self.reorder_meshes();

            self.base_mut().set_process_internal(true);

            self.signals().loaded().emit();
        }
        // reconnect all mask viewport textures to the dependent mesh shaders
        // this is necessary because Viewport texture paths are relative to the absolute scene tree
        else if what == CanvasItemNotification::ENTER_TREE {
            let mask_group = self.base().get_node_as::<Node>("Masks");
            for mask in mask_group
                .get_children()
                .iter_shared()
                .map(|n| n.cast::<SubViewport>())
            {
                for np in mask
                    .get_meta("meshes")
                    .to::<VarArray>()
                    .iter_shared()
                    .map(|v| v.to::<NodePath>())
                {
                    let node = self.base().get_node_as::<MeshInstance2D>(&np);
                    let mut mat = node.get_material().unwrap().cast::<ShaderMaterial>();
                    mat.set_shader_parameter("tex_mask", &mask.get_texture().unwrap().to_variant());
                }
            }
            /* Bug: godot-rust has not properly scoped this for public access
            RenderingServer::singleton().canvas_item_set_custom_rect_full(
                self.base().get_canvas_item(), true,
                Rect2i {
                    position: Vector2i::ZERO,
                    size: self.size
                }
            );
            */
        }
        // apply mutators to pose and send to driver
        else if what == CanvasItemNotification::INTERNAL_PROCESS && self.is_loaded() {
            let starting_pose = &mut {
                let mut state = Pose::with_map(self.pose_map.clone());
                state.populate(1.0);
                let current = self.pose.as_ref().unwrap().to_owned();
                state.update(&current);
                state
            };

            let output_pose =
                self.base_mut()
                    .get_children()
                    .iter_shared()
                    .fold(starting_pose, |state, child| {
                        if let Ok(mut mutator) = child.try_dynify::<dyn IMutator>() {
                            mutator.dyn_bind_mut().apply(state);
                        }
                        state
                    });
            self.model.as_mut().unwrap().driver.apply_pose(&output_pose);
            self.last_pose = Some(output_pose.clone());

            let drive_start = self.profile_enabled.then(Instant::now);
            self.drive_model();
            let drive_ns = drive_start.map(|start| start.elapsed().as_nanos()).unwrap_or(0);

            let mesh_submit_start = self.profile_enabled.then(Instant::now);
            let mesh_profile = self.update_meshes(false);
            let mesh_submit_ns = mesh_submit_start
                .map(|start| start.elapsed().as_nanos())
                .unwrap_or(0);

            let masks_start = self.profile_enabled.then(Instant::now);
            self.update_masks();
            let masks_ns = masks_start
                .map(|start| start.elapsed().as_nanos())
                .unwrap_or(0);

            let reorder_start = self.profile_enabled.then(Instant::now);
            self.reorder_meshes();
            let reorder_ns = reorder_start
                .map(|start| start.elapsed().as_nanos())
                .unwrap_or(0);

            if self.profile_enabled {
                self.record_frame_profile(
                    drive_ns,
                    mesh_submit_ns,
                    mesh_profile,
                    masks_ns,
                    reorder_ns,
                );
            }
        }
    }

    fn on_set(&mut self, property: StringName, value: Variant) -> bool {
        if !self.is_loaded() {
            return false;
        }

        // check if attempting to set a value on the internal ayagami driver
        if let (Some(pose), Some(key), Ok(v)) = (
            self.pose.as_mut(),
            param_to_key(property).as_ref(),
            value.try_to::<f32>(),
        ) {
            if let Some(p) = pose.get_mut_flattened(key) {
                *p = Value::opaque(v);
                return true;
            }
        }
        return false;
    }

    fn on_get(&self, property: StringName) -> Option<Variant> {
        if !self.is_loaded() {
            return None;
        }

        if property.begins_with(PARAMETER_PREFIX) {
            if property.ends_with(PARAMETER_RANGE_SUFFIX) {
                let key = key_param(
                    property
                        .trim_suffix(PARAMETER_RANGE_SUFFIX)
                        .to_string_name(),
                );
                if let Some((_, p)) = self.pose_map.get(&key) {
                    return Some(Vector2 { x: p.min, y: p.max }.to_variant());
                }
            } else if property.ends_with(PARAMETER_DEFAULT_SUFFIX) {
                let key = key_param(
                    property
                        .trim_suffix(PARAMETER_DEFAULT_SUFFIX)
                        .to_string_name(),
                );
                if let Some((_, p)) = self.pose_map.get(&key) {
                    return Some(p.default.to_variant());
                }
            } else if property.ends_with(PARAMETER_GROUP_SUFFIX) {
                let key = key_param(
                    property
                        .trim_suffix(PARAMETER_GROUP_SUFFIX)
                        .to_string_name(),
                );
                if let Some((_, p)) = self.pose_map.get(&key) {
                    return self
                        .parameter_groups
                        .get(&(p.uid as u32))
                        .map(|v| v.to_variant());
                }
            }
        }
        if property.begins_with(OUTPUT_PREFIX) {
            if let Some(key) = param_to_key(property.trim_prefix(OUTPUT_PREFIX).to_string_name()) {
                if let (Some(pose), Some(last)) = (self.pose.as_ref(), self.last_pose.as_ref()) {
                    return last
                        .get(&key)
                        .map(|v| v.value)
                        .or(pose.get_flattened(&key))
                        .map(|v| v.to_variant());
                } else {
                    return Some(0.0.to_variant());
                }
            }
        }
        if let Some(k) = param_to_key(property) {
            if let Some(pose) = self.pose.as_ref() {
                if let Some(value) = pose.get_flattened(&k) {
                    return Some(value.to_variant());
                }
            }
        }

        return None;
    }

    fn on_get_property_list(&mut self) -> Vec<PropertyInfo> {
        if !self.is_loaded() {
            return Vec::new();
        }

        // expose driver parameters as fields on the model
        self.pose_map
            .descriptors()
            .into_iter()
            .flat_map(|param| match param.key.clone() {
                Key::Param(id) => vec![
                    PropertyInfo {
                        variant_type: VariantType::FLOAT,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!("{}{}", PARAMETER_PREFIX, id).to_string_name(),
                        hint_info: PropertyHintInfo {
                            hint: PropertyHint::RANGE,
                            hint_string: format!("{},{}", param.min, param.max).to_gstring(),
                        },
                        usage: PropertyUsageFlags::STORAGE | PropertyUsageFlags::EDITOR,
                    },
                    PropertyInfo {
                        variant_type: VariantType::VECTOR2,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!(
                            "{}{}{}",
                            PARAMETER_PREFIX, id, PARAMETER_RANGE_SUFFIX
                        )
                        .to_string_name(),
                        hint_info: PropertyHintInfo::none(),
                        usage: PropertyUsageFlags::READ_ONLY | PropertyUsageFlags::EDITOR,
                    },
                    PropertyInfo {
                        variant_type: VariantType::FLOAT,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!(
                            "{}{}{}",
                            PARAMETER_PREFIX, id, PARAMETER_DEFAULT_SUFFIX
                        )
                        .to_string_name(),
                        hint_info: PropertyHintInfo::none(),
                        usage: PropertyUsageFlags::READ_ONLY | PropertyUsageFlags::EDITOR,
                    },
                    PropertyInfo {
                        variant_type: VariantType::STRING,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!(
                            "{}{}{}",
                            PARAMETER_PREFIX, id, PARAMETER_GROUP_SUFFIX
                        )
                        .to_string_name(),
                        hint_info: PropertyHintInfo::none(),
                        usage: PropertyUsageFlags::READ_ONLY | PropertyUsageFlags::EDITOR,
                    },
                    PropertyInfo {
                        variant_type: VariantType::FLOAT,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!("{}{}{}", OUTPUT_PREFIX, PARAMETER_PREFIX, id)
                            .to_string_name(),
                        hint_info: PropertyHintInfo::none(),
                        usage: PropertyUsageFlags::READ_ONLY | PropertyUsageFlags::EDITOR,
                    },
                ],
                Key::Part(id) => vec![
                    PropertyInfo {
                        variant_type: VariantType::FLOAT,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!("{}{}", PART_PREFIX, id).to_string_name(),
                        hint_info: PropertyHintInfo {
                            hint: PropertyHint::RANGE,
                            hint_string: format!("{},{}", 0.0, 1.0).to_gstring(),
                        },
                        usage: PropertyUsageFlags::STORAGE | PropertyUsageFlags::EDITOR,
                    },
                    PropertyInfo {
                        variant_type: VariantType::FLOAT,
                        class_name: ClassId::none().to_string_name(),
                        property_name: format!("{}{}{}", OUTPUT_PREFIX, PART_PREFIX, id)
                            .to_string_name(),
                        hint_info: PropertyHintInfo::none(),
                        usage: PropertyUsageFlags::READ_ONLY | PropertyUsageFlags::EDITOR,
                    },
                ],
            })
            .collect()
    }

    fn on_property_get_revert(&self, property: StringName) -> Option<Variant> {
        if self.model.is_none() {
            return None;
        }

        if let Some(key) = param_to_key(property.clone()) {
            if let Some((_, p)) = self.pose_map.get(&key) {
                return Some(p.default.to_variant());
            }
        }

        return None;
    }
}
