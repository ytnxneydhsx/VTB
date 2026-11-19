#pragma once

class QApplication;

namespace vtb::desktop {

enum class ColorMode { LIGHT, DARK };

namespace metrics {
    inline constexpr int kSpacing = 16;
    inline constexpr int kContentMargin = 28;
    inline constexpr int kSidebarWidth = 200;
    inline constexpr int kSwitchWidth = 64;
    inline constexpr int kSwitchHeight = 42;
    inline constexpr int kSwitchTransitionMs = 170;
} // namespace metrics

auto applyTheme(QApplication& application, ColorMode mode) -> void;

} // namespace vtb::desktop
