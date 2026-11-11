#include "live2d_render_session.h"

#include "core/config/engine.h"
#include "core/math/math_funcs.h"
#include "core/os/memory.h"
#include "core/os/thread.h"
#include "core/variant/array.h"
#include "scene/2d/node_2d.h"
#include "scene/animation/animation_player.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_device_commons.h"
#include "servers/rendering/rendering_server.h"
#include "vtb/live2d/live2d_parameters.hpp"

namespace vtb {

namespace {

constexpr float kMinimumViewZoom = 0.5F;
constexpr float kMaximumViewZoom = 6.0F;
constexpr float kMinimumViewOffset = -2.0F;
constexpr float kMaximumViewOffset = 2.0F;

void configure_mix_attachment(Ref<RDPipelineColorBlendStateAttachment> &r_attachment) {
	r_attachment.instantiate();
	r_attachment->set_enable_blend(true);
	r_attachment->set_src_color_blend_factor(RD::BLEND_FACTOR_ONE);
	r_attachment->set_dst_color_blend_factor(RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	r_attachment->set_src_alpha_blend_factor(RD::BLEND_FACTOR_ONE);
	r_attachment->set_dst_alpha_blend_factor(RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
}

void configure_add_attachment(Ref<RDPipelineColorBlendStateAttachment> &r_attachment) {
	r_attachment.instantiate();
	r_attachment->set_enable_blend(true);
	r_attachment->set_src_color_blend_factor(RD::BLEND_FACTOR_ONE);
	r_attachment->set_dst_color_blend_factor(RD::BLEND_FACTOR_ONE);
	r_attachment->set_src_alpha_blend_factor(RD::BLEND_FACTOR_ZERO);
	r_attachment->set_dst_alpha_blend_factor(RD::BLEND_FACTOR_ONE);
}

void configure_multiply_attachment(Ref<RDPipelineColorBlendStateAttachment> &r_attachment) {
	r_attachment.instantiate();
	r_attachment->set_enable_blend(true);
	r_attachment->set_src_color_blend_factor(RD::BLEND_FACTOR_DST_COLOR);
	r_attachment->set_dst_color_blend_factor(RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	r_attachment->set_src_alpha_blend_factor(RD::BLEND_FACTOR_ZERO);
	r_attachment->set_dst_alpha_blend_factor(RD::BLEND_FACTOR_ONE);
}

} // namespace

Live2DRenderViewState::Live2DRenderViewState(float p_zoom, float p_offset_x, float p_offset_y) :
		zoom_(p_zoom),
		offset_x_(p_offset_x),
		offset_y_(p_offset_y) {
}

float Live2DRenderViewState::zoom() const noexcept {
	return zoom_;
}

float Live2DRenderViewState::offset_x() const noexcept {
	return offset_x_;
}

float Live2DRenderViewState::offset_y() const noexcept {
	return offset_y_;
}

Live2DRenderSession::Live2DRenderSession() = default;

Live2DRenderSession::~Live2DRenderSession() {
	shutdown();
}

Error Live2DRenderSession::initialize(const Live2DRenderSessionConfig &p_config) {
	if (state_ != Live2DRenderSessionState::Created) {
		return fail(ERR_ALREADY_IN_USE, "Live2D render session initialize may only be called once");
	}
	Error thread_error = ensure_main_thread("initialize");
	if (thread_error != OK) {
		return thread_error;
	}
	if (p_config.model_path.is_empty() || p_config.output_size.x <= 0 ||
			p_config.output_size.y <= 0 || p_config.content_scale <= 0.0F) {
		return fail(ERR_INVALID_PARAMETER, "Live2D render session configuration is invalid");
	}

	scene_tree_ = SceneTree::get_singleton();
	if (scene_tree_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "Godot SceneTree must be initialized before Live2DRenderSession");
	}

	config_ = p_config;
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return fail(ERR_UNCONFIGURED, "Godot RenderingServer is unavailable");
	}
	previous_clear_color_ = rendering_server->get_default_clear_color();
	rendering_server->set_default_clear_color(config_.clear_color);
	clear_color_changed_ = true;

	Error blend_error = register_blend_modes();
	if (blend_error != OK) {
		return blend_error;
	}
	Error scene_error = create_scene_nodes();
	if (scene_error != OK) {
		return scene_error;
	}

	state_ = Live2DRenderSessionState::Initialized;
	last_error_ = String();
	return OK;
}

Error Live2DRenderSession::load_model() {
	if (state_ != Live2DRenderSessionState::Initialized) {
		return fail(ERR_UNCONFIGURED, "Live2D model loading requires an initialized session");
	}
	Error thread_error = ensure_main_thread("load_model");
	if (thread_error != OK) {
		return thread_error;
	}

	Engine *engine = Engine::get_singleton();
	const StringName loader_name("AyagamiLoader");
	if (engine == nullptr || !engine->has_singleton(loader_name)) {
		return fail(ERR_UNAVAILABLE, "AyagamiLoader singleton is unavailable");
	}
	Object *loader = engine->get_singleton_object(loader_name);
	if (loader == nullptr || !loader->has_method("load_model") ||
			!loader->has_method("load_motion_library")) {
		return fail(ERR_UNAVAILABLE, "AyagamiLoader does not expose the required model and motion APIs");
	}

	Variant model_variant = loader->call("load_model", config_.model_path);
	Object *model_object = model_variant.get_validated_object();
	model_ = Object::cast_to<Node2D>(model_object);
	if (model_ == nullptr) {
		return fail(ERR_CANT_OPEN, "AyagamiLoader failed to create an Ayagami model node");
	}
	viewport_->add_child(model_);

	Error parameter_error = cache_model_parameters();
	if (parameter_error != OK) {
		return parameter_error;
	}

	bool size_valid = false;
	Variant model_size_variant = model_->get("size", &size_valid);
	if (!size_valid || model_size_variant.get_type() != Variant::VECTOR2I) {
		return fail(ERR_INVALID_DATA, "Ayagami model size property is unavailable");
	}
	Vector2i model_size = model_size_variant;
	if (model_size.x <= 0 || model_size.y <= 0) {
		return fail(ERR_INVALID_DATA, "Ayagami model size is invalid");
	}
	const float horizontal_scale =
			(static_cast<float>(config_.output_size.x) * config_.content_scale) /
			static_cast<float>(model_size.x);
	const float vertical_scale =
			(static_cast<float>(config_.output_size.y) * config_.content_scale) /
			static_cast<float>(model_size.y);
	base_model_scale_ = horizontal_scale;
	if (vertical_scale < horizontal_scale) {
		base_model_scale_ = vertical_scale;
	}

	Node *motion_node = model_->get_node_or_null(NodePath("MotionController"));
	motion_controller_ = Object::cast_to<AnimationPlayer>(motion_node);
	if (motion_controller_ == nullptr) {
		return fail(ERR_DOES_NOT_EXIST, "Ayagami model has no MotionController AnimationPlayer");
	}

	Variant library_variant = loader->call("load_motion_library", model_);
	Object *library_object = library_variant.get_validated_object();
	AnimationLibrary *library = Object::cast_to<AnimationLibrary>(library_object);
	if (library == nullptr) {
		return fail(ERR_CANT_OPEN, "Ayagami motion library loading failed");
	}
	motion_library_ = Ref<AnimationLibrary>(library);
	const StringName default_library;
	if (motion_controller_->has_animation_library(default_library)) {
		motion_controller_->remove_animation_library(default_library);
	}
	Error library_error = motion_controller_->add_animation_library(default_library, motion_library_);
	if (library_error != OK) {
		return fail(library_error, "Ayagami motion library could not be attached to MotionController");
	}
	if (!config_.startup_motion.is_empty() &&
			!motion_controller_->has_animation(config_.startup_motion)) {
		return fail(ERR_DOES_NOT_EXIST, "Configured Ayagami startup motion is unavailable");
	}

	Error view_error = apply_view_transform();
	if (view_error != OK) {
		return view_error;
	}
	Error parameter_apply_error = apply_parameters(Live2DParameters());
	if (parameter_apply_error != OK) {
		return parameter_apply_error;
	}

	state_ = Live2DRenderSessionState::Ready;
	last_error_ = String();
	return OK;
}

Error Live2DRenderSession::start() {
	if (state_ != Live2DRenderSessionState::Ready) {
		return fail(ERR_UNCONFIGURED, "Live2D render session start requires a ready model");
	}
	Error thread_error = ensure_main_thread("start");
	if (thread_error != OK) {
		return thread_error;
	}
	state_ = Live2DRenderSessionState::Running;
	if (!config_.startup_motion.is_empty()) {
		Error motion_error = play_motion(config_.startup_motion);
		if (motion_error != OK) {
			return motion_error;
		}
	}
	last_error_ = String();
	return OK;
}

Error Live2DRenderSession::submit_parameters(const Live2DParameters &p_parameters) {
	if (state_ != Live2DRenderSessionState::Running) {
		return ERR_UNCONFIGURED;
	}
	Error thread_error = ensure_main_thread("submit_parameters");
	if (thread_error != OK) {
		return thread_error;
	}

	Error error = apply_parameters(p_parameters);
	if (error != OK) {
		return fail(error, "Ayagami rejected a Live2D parameter update");
	}
	last_error_ = String();
	return OK;
}

Error Live2DRenderSession::apply_parameters(const Live2DParameters &p_parameters) {
	Error error = set_parameter_if_available("ParamAngleX", p_parameters.angle_x());
	if (error == OK) {
		error = set_parameter_if_available("ParamAngleY", p_parameters.angle_y());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamAngleZ", p_parameters.angle_z());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamBodyAngleX", p_parameters.body_angle_x());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamBodyAngleY", p_parameters.body_angle_y());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamBodyAngleZ", p_parameters.body_angle_z());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamEyeLOpen", p_parameters.left_eye_open());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamEyeROpen", p_parameters.right_eye_open());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamMouthOpenY", p_parameters.mouth_open());
	}
	if (error == OK) {
		error = set_parameter_if_available("ParamMouthForm", p_parameters.mouth_form());
	}
	return error;
}

Error Live2DRenderSession::set_view_state(const Live2DRenderViewState &p_view_state) {
	if (state_ != Live2DRenderSessionState::Ready &&
			state_ != Live2DRenderSessionState::Running) {
		return fail(ERR_UNCONFIGURED, "Live2D view transform requires a loaded model");
	}
	Error thread_error = ensure_main_thread("set_view_state");
	if (thread_error != OK) {
		return thread_error;
	}
	view_state_ = Live2DRenderViewState(
			CLAMP(p_view_state.zoom(), kMinimumViewZoom, kMaximumViewZoom),
			CLAMP(p_view_state.offset_x(), kMinimumViewOffset, kMaximumViewOffset),
			CLAMP(p_view_state.offset_y(), kMinimumViewOffset, kMaximumViewOffset));
	return apply_view_transform();
}

Error Live2DRenderSession::play_motion(const StringName &p_motion_name) {
	if (state_ != Live2DRenderSessionState::Running || motion_controller_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "Ayagami motion playback requires a running session");
	}
	Error thread_error = ensure_main_thread("play_motion");
	if (thread_error != OK) {
		return thread_error;
	}
	if (!motion_controller_->has_animation(p_motion_name)) {
		return fail(ERR_DOES_NOT_EXIST, "Requested Ayagami motion is unavailable");
	}
	if (motion_controller_->has_animation("RESET")) {
		motion_controller_->play("RESET");
		motion_controller_->advance(0.001);
	}
	motion_controller_->play(p_motion_name);
	last_error_ = String();
	return OK;
}

void Live2DRenderSession::shutdown() noexcept {
	if (state_ == Live2DRenderSessionState::Stopped) {
		return;
	}
	if (!Thread::is_main_thread()) {
		last_error_ = "Live2D render session shutdown must run on the Godot main thread";
		state_ = Live2DRenderSessionState::Failed;
		return;
	}
	if (motion_controller_ != nullptr) {
		motion_controller_->stop();
	}
	release_scene_nodes();
	unregister_blend_modes();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (clear_color_changed_ && rendering_server != nullptr) {
		rendering_server->set_default_clear_color(previous_clear_color_);
	}
	clear_color_changed_ = false;
	scene_tree_ = nullptr;
	state_ = Live2DRenderSessionState::Stopped;
}

Live2DRenderSessionState Live2DRenderSession::state() const noexcept {
	return state_;
}

const String &Live2DRenderSession::last_error() const noexcept {
	return last_error_;
}

RID Live2DRenderSession::viewport_rid() const {
	if (viewport_ == nullptr) {
		return RID();
	}
	return viewport_->get_viewport_rid();
}

RID Live2DRenderSession::texture_rid() const {
	RID viewport = viewport_rid();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (!viewport.is_valid() || rendering_server == nullptr) {
		return RID();
	}
	return rendering_server->viewport_get_texture(viewport);
}

Error Live2DRenderSession::ensure_main_thread(const String &p_operation) {
	if (Thread::is_main_thread()) {
		return OK;
	}
	return fail(ERR_UNAVAILABLE, p_operation + " must run on the Godot main thread");
}

Error Live2DRenderSession::fail(Error p_error, const String &p_message) {
	last_error_ = p_message;
	state_ = Live2DRenderSessionState::Failed;
	return p_error;
}

Error Live2DRenderSession::register_blend_modes() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return fail(ERR_UNCONFIGURED, "RenderingServer is unavailable for Ayagami blend registration");
	}
	configure_mix_attachment(mix_attachment_);
	configure_add_attachment(add_attachment_);
	configure_multiply_attachment(multiply_attachment_);
	rendering_server->register_blend_mode(
			RSE::SHADER_CANVAS_ITEM, "ayagami_mix", mix_attachment_.ptr());
	rendering_server->register_blend_mode(
			RSE::SHADER_CANVAS_ITEM, "ayagami_add", add_attachment_.ptr());
	rendering_server->register_blend_mode(
			RSE::SHADER_CANVAS_ITEM, "ayagami_mul", multiply_attachment_.ptr());
	blend_modes_registered_ = true;
	return OK;
}

void Live2DRenderSession::unregister_blend_modes() noexcept {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (blend_modes_registered_ && rendering_server != nullptr) {
		rendering_server->unregister_blend_mode(RSE::SHADER_CANVAS_ITEM, "ayagami_mul");
		rendering_server->unregister_blend_mode(RSE::SHADER_CANVAS_ITEM, "ayagami_add");
		rendering_server->unregister_blend_mode(RSE::SHADER_CANVAS_ITEM, "ayagami_mix");
	}
	blend_modes_registered_ = false;
	multiply_attachment_.unref();
	add_attachment_.unref();
	mix_attachment_.unref();
}

Error Live2DRenderSession::create_scene_nodes() {
	if (scene_tree_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "SceneTree is unavailable while creating the Live2D scene subtree");
	}
	session_root_ = memnew(Node2D);
	session_root_->set_name("VTBLive2DRenderSession");
	scene_tree_->get_root()->add_child(session_root_);

	viewport_ = memnew(SubViewport);
	viewport_->set_name("Live2DEmbeddedViewport");
	viewport_->set_size(config_.output_size);
#ifndef _3D_DISABLED
	viewport_->set_disable_3d(true);
#endif
	viewport_->set_transparent_background(false);
	viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
	session_root_->add_child(viewport_);
	return OK;
}

Error Live2DRenderSession::cache_model_parameters() {
	if (model_ == nullptr || !model_->has_method("get_parameters")) {
		return fail(ERR_UNAVAILABLE, "Ayagami model does not expose get_parameters");
	}
	Variant parameter_variant = model_->call("get_parameters");
	if (parameter_variant.get_type() != Variant::ARRAY) {
		return fail(ERR_INVALID_DATA, "Ayagami get_parameters returned an invalid value");
	}
	parameter_ranges_.clear();
	Array parameters = parameter_variant;
	for (int index = 0; index < parameters.size(); ++index) {
		StringName parameter = parameters[index];
		bool range_valid = false;
		String range_property = "parameters/";
		range_property += String(parameter);
		range_property += "/range";
		Variant range_variant = model_->get(StringName(range_property), &range_valid);
		if (!range_valid || range_variant.get_type() != Variant::VECTOR2) {
			continue;
		}
		Vector2 range = range_variant;
		parameter_ranges_.insert(parameter, range);
	}
	if (parameter_ranges_.is_empty()) {
		return fail(ERR_INVALID_DATA, "Ayagami model exposes no usable parameter ranges");
	}
	return OK;
}

Error Live2DRenderSession::apply_view_transform() {
	if (model_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "Ayagami model is unavailable for view transform");
	}
	const float scale = base_model_scale_ * view_state_.zoom();
	model_->set_scale(Vector2(scale, scale));
	const Vector2 center(
			static_cast<float>(config_.output_size.x) * 0.5F,
			static_cast<float>(config_.output_size.y) * 0.5F);
	const Vector2 offset(
			static_cast<float>(config_.output_size.x) * view_state_.offset_x(),
			static_cast<float>(config_.output_size.y) * view_state_.offset_y());
	model_->set_position(center + offset);
	last_error_ = String();
	return OK;
}

Error Live2DRenderSession::set_parameter_if_available(
		const StringName &p_parameter, float p_value) {
	const Vector2 *range = parameter_ranges_.getptr(p_parameter);
	if (range == nullptr) {
		return OK;
	}
	const float clamped_value = CLAMP(p_value, range->x, range->y);
	bool property_valid = false;
	model_->set(parameter_property(p_parameter), clamped_value, &property_valid);
	if (!property_valid) {
		return ERR_INVALID_DATA;
	}
	return OK;
}

StringName Live2DRenderSession::parameter_property(const StringName &p_parameter) const {
	String property = "parameters/";
	property += String(p_parameter);
	return StringName(property);
}

void Live2DRenderSession::release_scene_nodes() noexcept {
	motion_controller_ = nullptr;
	motion_library_.unref();
	parameter_ranges_.clear();
	model_ = nullptr;
	viewport_ = nullptr;
	if (session_root_ != nullptr) {
		Node *parent = session_root_->get_parent();
		if (parent != nullptr) {
			parent->remove_child(session_root_);
		}
		memdelete(session_root_);
		session_root_ = nullptr;
	}
}

} // namespace vtb
