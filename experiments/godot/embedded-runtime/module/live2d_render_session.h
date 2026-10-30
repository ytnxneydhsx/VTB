#pragma once

#include "core/error/error_list.h"
#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/object/ref_counted.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid.h"
#include "scene/resources/animation_library.h"
#include "servers/rendering/rendering_device_binds.h"

class AnimationPlayer;
class Node2D;
class SceneTree;
class SubViewport;

namespace vtb {

class Live2DParameters;

enum class Live2DRenderSessionState {
	Created,
	Initialized,
	Ready,
	Running,
	Failed,
	Stopped,
};

class Live2DRenderSessionConfig {
public:
	Live2DRenderSessionConfig() = default;
	~Live2DRenderSessionConfig() = default;

	Live2DRenderSessionConfig(const Live2DRenderSessionConfig &) = default;
	Live2DRenderSessionConfig &operator=(const Live2DRenderSessionConfig &) = default;
	Live2DRenderSessionConfig(Live2DRenderSessionConfig &&) noexcept = default;
	Live2DRenderSessionConfig &operator=(Live2DRenderSessionConfig &&) noexcept = default;

	String model_path;
	StringName startup_motion;
	Vector2i output_size = Vector2i(1920, 1080);
	float content_scale = 0.92F;
	Color clear_color = Color(0.18F, 0.18F, 0.18F, 1.0F);
};

class Live2DRenderViewState {
public:
	Live2DRenderViewState() = default;
	Live2DRenderViewState(float p_zoom, float p_offset_x, float p_offset_y);
	~Live2DRenderViewState() = default;

	Live2DRenderViewState(const Live2DRenderViewState &) = default;
	Live2DRenderViewState &operator=(const Live2DRenderViewState &) = default;
	Live2DRenderViewState(Live2DRenderViewState &&) noexcept = default;
	Live2DRenderViewState &operator=(Live2DRenderViewState &&) noexcept = default;

	float zoom() const noexcept;
	float offset_x() const noexcept;
	float offset_y() const noexcept;

private:
	float zoom_ = 1.0F;
	float offset_x_ = 0.0F;
	float offset_y_ = 0.0F;
};

class Live2DRenderSession {
public:
	Live2DRenderSession();
	~Live2DRenderSession();

	Live2DRenderSession(const Live2DRenderSession &) = delete;
	Live2DRenderSession &operator=(const Live2DRenderSession &) = delete;
	Live2DRenderSession(Live2DRenderSession &&) = delete;
	Live2DRenderSession &operator=(Live2DRenderSession &&) = delete;

	Error initialize(const Live2DRenderSessionConfig &p_config);
	Error load_model();
	Error start();
	Error submit_parameters(const Live2DParameters &p_parameters);
	Error set_view_state(const Live2DRenderViewState &p_view_state);
	Error play_motion(const StringName &p_motion_name);
	void shutdown() noexcept;

	Live2DRenderSessionState state() const noexcept;
	const String &last_error() const noexcept;
	RID viewport_rid() const;
	RID texture_rid() const;

private:
	Error ensure_main_thread(const String &p_operation);
	Error fail(Error p_error, const String &p_message);
	Error register_blend_modes();
	void unregister_blend_modes() noexcept;
	Error create_scene_nodes();
	Error cache_model_parameters();
	Error apply_view_transform();
	Error apply_parameters(const Live2DParameters &p_parameters);
	Error set_parameter_if_available(const StringName &p_parameter, float p_value);
	StringName parameter_property(const StringName &p_parameter) const;
	void release_scene_nodes() noexcept;

	Live2DRenderSessionConfig config_;
	Live2DRenderSessionState state_ = Live2DRenderSessionState::Created;
	String last_error_;

	SceneTree *scene_tree_ = nullptr;
	Node2D *session_root_ = nullptr;
	SubViewport *viewport_ = nullptr;
	Node2D *model_ = nullptr;
	AnimationPlayer *motion_controller_ = nullptr;
	Ref<AnimationLibrary> motion_library_;
	HashMap<StringName, Vector2> parameter_ranges_;

	Ref<RDPipelineColorBlendStateAttachment> mix_attachment_;
	Ref<RDPipelineColorBlendStateAttachment> add_attachment_;
	Ref<RDPipelineColorBlendStateAttachment> multiply_attachment_;
	bool blend_modes_registered_ = false;
	Color previous_clear_color_;
	bool clear_color_changed_ = false;

	float base_model_scale_ = 1.0F;
	Live2DRenderViewState view_state_;
};

} // namespace vtb
