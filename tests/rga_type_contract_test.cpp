#include <type_traits>

#include "vtb/hardware/rga/rga_buffer_pool.hpp"
#include "vtb/hardware/rga/rga_device.hpp"
#include "vtb/hardware/rga/rga_model_input_transformer.hpp"
#include "vtb/hardware/rga/rga_nv12_frame_transformer.hpp"
#include "vtb/hardware/rga/rga_transform_session.hpp"
#include "vtb/hardware/rga/rga_transformer.hpp"
#include "vtb/render/nv12_frame_sink.hpp"

static_assert(!std::is_copy_constructible_v<vtb::RgaDevice>);
static_assert(!std::is_move_constructible_v<vtb::RgaDevice>);
static_assert(!std::is_copy_constructible_v<vtb::RgaBufferPool>);
static_assert(!std::is_move_constructible_v<vtb::RgaBufferPool>);
static_assert(!std::is_copy_constructible_v<vtb::RgaTransformer>);
static_assert(!std::is_move_constructible_v<vtb::RgaTransformer>);
static_assert(!std::is_copy_constructible_v<vtb::RgaTransformSession>);
static_assert(!std::is_move_constructible_v<vtb::RgaTransformSession>);
static_assert(!std::is_copy_constructible_v<vtb::RgaModelInputTransformer>);
static_assert(!std::is_move_constructible_v<vtb::RgaModelInputTransformer>);
static_assert(!std::is_copy_constructible_v<vtb::RgaNv12FrameTransformer>);
static_assert(!std::is_move_constructible_v<vtb::RgaNv12FrameTransformer>);

static_assert(!std::is_copy_constructible_v<vtb::RgaTransformLease>);
static_assert(std::is_nothrow_move_constructible_v<vtb::RgaTransformLease>);
static_assert(std::is_nothrow_move_assignable_v<vtb::RgaTransformLease>);
static_assert(!std::is_copy_constructible_v<vtb::Nv12FrameLease>);
static_assert(std::is_nothrow_move_constructible_v<vtb::Nv12FrameLease>);
static_assert(std::is_nothrow_move_assignable_v<vtb::Nv12FrameLease>);

int main() {
    return 0;
}
