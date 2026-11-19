#include "board_monitor_page.hh"

#include "board_monitor.hh"
#include "theme.hh"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>

#include <cmath>

namespace vtb::desktop {
namespace {

    constexpr auto kUnavailable = "—";
    constexpr double kGiB = 1073741824.0;

    class SensorLabel {
    public:
        const char* type;
        const char* label;
    };

    constexpr std::array kSensors{
        SensorLabel{ "bigcore0-thermal", "大核簇 0" },
        SensorLabel{ "bigcore1-thermal", "大核簇 1" },
        SensorLabel{ "littlecore-thermal", "小核簇" },
        SensorLabel{ "gpu-thermal", "GPU" },
        SensorLabel{ "npu-thermal", "NPU" },
        SensorLabel{ "center-thermal", "中心" },
    };

    auto label(const QString& value, const char* role = "") -> QLabel* {
        auto* widget = new QLabel(value);
        widget->setProperty("role", role);
        widget->setTextFormat(Qt::PlainText);
        return widget;
    }

    auto value_label(const char* role = "") -> QLabel* {
        return label(QString::fromUtf8(kUnavailable), role);
    }

    auto panel(QBoxLayout& parent, const QString& title) -> QVBoxLayout* {
        auto* frame = new QFrame;
        frame->setProperty("panel", "surface");
        auto* content = new QVBoxLayout(frame);
        content->setContentsMargins(16, 12, 16, 14);
        content->setSpacing(6);
        if (!title.isEmpty()) {
            content->addWidget(label(title, "heading"));
        }
        parent.addWidget(frame, 1);
        return content;
    }

    auto meter(const QString& name) -> QProgressBar* {
        auto* widget = new QProgressBar;
        widget->setObjectName(QStringLiteral("monitorMeter"));
        widget->setAccessibleName(name);
        widget->setRange(0, 100);
        widget->setValue(0);
        widget->setTextVisible(false);
        widget->setFixedHeight(7);
        return widget;
    }

    auto set_load(QLabel& value, QProgressBar& bar, double percentage, bool waiting = false)
        -> void {
        if (percentage >= 0 && percentage <= 100) {
            const auto rounded = static_cast<int>(std::lround(percentage));
            value.setText(QStringLiteral("%1%").arg(rounded));
            bar.setValue(rounded);
            return;
        }
        bar.setValue(0);
        if (waiting) {
            value.setText(QStringLiteral("采样中"));
        } else {
            value.setText(QString::fromUtf8(kUnavailable));
        }
    }

    auto temperature(const QJsonValue& value) -> QString {
        if (value.isDouble()) {
            return QStringLiteral("%1 °C").arg(value.toDouble() / 1000.0, 0, 'f', 1);
        }
        return QString::fromUtf8(kUnavailable);
    }

    auto frequency_mhz(const QJsonValue& value) -> QString {
        const auto hz = value.toInteger();
        if (hz > 0) {
            const auto mhz = QLocale().toString(static_cast<double>(hz) / 1e6, 'f', 0);
            return QStringLiteral("%1 MHz").arg(mhz);
        }
        return QString::fromUtf8(kUnavailable);
    }

} // namespace

BoardMonitorPage::BoardMonitorPage(QWidget* parent)
    : QScrollArea(parent) {
    setObjectName(QStringLiteral("boardMonitorPage"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("page"));
    setWidget(body);
    auto* content = new QVBoxLayout(body);
    content->setContentsMargins(
        metrics::kContentMargin, metrics::kContentMargin, metrics::kContentMargin,
        metrics::kContentMargin);
    content->setSpacing(metrics::kSpacing);

    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("板卡监控"), "title"));
    heading->addStretch();
    stream_state_ = label(QStringLiteral("等待板端采样"), "muted");
    stream_state_->setObjectName(QStringLiteral("monitorStreamState"));
    heading->addWidget(stream_state_, 0, Qt::AlignVCenter);
    content->addLayout(heading);

    auto* overview = new QHBoxLayout;
    overview->setSpacing(metrics::kSpacing);
    content->addLayout(overview);

    auto* cpu = panel(*overview, {});
    cpu->addWidget(label(QStringLiteral("CPU 总占用"), "sub"));
    cpu_total_ = value_label("monitorValue");
    cpu_total_->setObjectName(QStringLiteral("monitorCpuTotal"));
    cpu->addWidget(cpu_total_);
    cpu_meter_ = meter(QStringLiteral("CPU 总占用"));
    cpu->addWidget(cpu_meter_);

    auto* ram = panel(*overview, {});
    ram->addWidget(label(QStringLiteral("RAM"), "sub"));
    auto* memory_values = new QHBoxLayout;
    memory_values->setSpacing(6);
    memory_used_ = value_label("monitorValue");
    memory_total_ = label({}, "muted");
    memory_usage_ = value_label("muted");
    memory_values->addWidget(memory_used_, 0, Qt::AlignBaseline);
    memory_values->addWidget(memory_total_, 0, Qt::AlignBaseline);
    memory_values->addStretch();
    memory_values->addWidget(memory_usage_, 0, Qt::AlignBaseline);
    ram->addLayout(memory_values);
    memory_meter_ = meter(QStringLiteral("RAM 使用率"));
    ram->addWidget(memory_meter_);

    auto* soc = panel(*overview, {});
    soc->addWidget(label(QStringLiteral("SoC 温度"), "sub"));
    soc_temperature_ = value_label("monitorValue");
    soc->addWidget(soc_temperature_);
    soc->addWidget(label(QStringLiteral("soc-thermal"), "muted"));

    auto* details = new QHBoxLayout;
    details->setSpacing(metrics::kSpacing);
    auto* left = new QVBoxLayout;
    auto* right = new QVBoxLayout;
    right->setSpacing(metrics::kSpacing);
    details->addLayout(left, 6);
    details->addLayout(right, 5);
    content->addLayout(details);
    content->addStretch();

    auto* cpu_details = panel(*left, {});
    auto* cpu_title = new QHBoxLayout;
    cpu_title->addWidget(label(QStringLiteral("CPU 逐核"), "heading"));
    cpu_title->addStretch();
    cpu_title->addWidget(label(QStringLiteral("8 核"), "muted"));
    cpu_details->addLayout(cpu_title);

    auto* cpu_table = new QGridLayout;
    cpu_table->setVerticalSpacing(3);
    cpu_table->setHorizontalSpacing(9);
    cpu_table->setColumnStretch(1, 1);
    cpu_table->addWidget(label(QStringLiteral("核心"), "muted"), 0, 0);
    cpu_table->addWidget(label(QStringLiteral("占用"), "muted"), 0, 1);
    cpu_table->addWidget(label(QStringLiteral("频率（GHz）"), "muted"), 0, 3, Qt::AlignRight);
    for (std::size_t id = 0; id < cores_.size(); ++id) {
        const auto name = QStringLiteral("CPU %1").arg(id);
        const int row = 1 + static_cast<int>(id) * 2;
        auto* divider = new QFrame;
        divider->setObjectName(QStringLiteral("monitorDivider"));
        divider->setFixedHeight(1);
        cpu_table->addWidget(divider, row, 0, 1, 4);
        auto& core = cores_[id];
        core.meter = meter(name);
        core.load = value_label("sub");
        core.load->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        core.load->setMinimumWidth(40);
        core.frequency = value_label("sub");
        auto* name_widget = label(name, "sub");
        name_widget->setMinimumHeight(22);
        cpu_table->addWidget(name_widget, row + 1, 0);
        cpu_table->addWidget(core.meter, row + 1, 1, Qt::AlignVCenter);
        cpu_table->addWidget(core.load, row + 1, 2);
        cpu_table->addWidget(core.frequency, row + 1, 3, Qt::AlignRight);
    }
    cpu_details->addLayout(cpu_table);
    left->addStretch();

    auto* gpu = panel(*right, {});
    auto* gpu_top = new QHBoxLayout;
    gpu_top->addWidget(label(QStringLiteral("GPU"), "heading"));
    gpu_top->addStretch();
    gpu_frequency_ = value_label("monitorFrequency");
    gpu_top->addWidget(gpu_frequency_);
    gpu->addLayout(gpu_top);
    auto* gpu_bottom = new QHBoxLayout;
    gpu_bottom->setSpacing(12);
    gpu_bottom->addWidget(label(QStringLiteral("负载"), "sub"));
    gpu_meter_ = meter(QStringLiteral("GPU 负载"));
    gpu_bottom->addWidget(gpu_meter_, 1, Qt::AlignVCenter);
    gpu_load_ = value_label("sub");
    gpu_load_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gpu_load_->setMinimumWidth(36);
    gpu_bottom->addWidget(gpu_load_);
    gpu->addLayout(gpu_bottom);

    auto* npu = panel(*right, {});
    auto* npu_top = new QHBoxLayout;
    npu_top->addWidget(label(QStringLiteral("NPU"), "heading"));
    npu_top->addStretch();
    npu_frequency_ = value_label("monitorFrequency");
    npu_top->addWidget(npu_frequency_);
    npu->addLayout(npu_top);
    npu->addWidget(label(QStringLiteral("当前频率"), "muted"));

    auto* temperature_details = panel(*right, QStringLiteral("温度传感器"));
    auto* sensor_grid = new QGridLayout;
    sensor_grid->setHorizontalSpacing(8);
    sensor_grid->setVerticalSpacing(3);
    for (std::size_t id = 0; id < kSensors.size(); ++id) {
        const auto col = static_cast<int>(id % 3);
        const auto row = static_cast<int>(id / 3);
        auto* name = label(QString::fromUtf8(kSensors[id].label), "muted");
        temperatures_[id] = value_label("sub");
        sensor_grid->addWidget(name, row * 2, col);
        sensor_grid->addWidget(temperatures_[id], row * 2 + 1, col);
        sensor_grid->setColumnStretch(col, 1);
    }
    temperature_details->addLayout(sensor_grid);
    right->addStretch();
}

auto BoardMonitorPage::showSnapshot(const QJsonObject& snapshot) -> void {
    const auto cpu = snapshot.value(QStringLiteral("cpu")).toObject();
    const auto entries = cpu.value(QStringLiteral("cores")).toArray();
    set_load(*cpu_total_, *cpu_meter_, cpu.value(QStringLiteral("usagePct")).toDouble(-1),
        !entries.isEmpty());
    std::array<QJsonObject, 8> core_data;
    for (const auto& entry : entries) {
        const auto data = entry.toObject();
        const auto id = data.value(QStringLiteral("id")).toInt(-1);
        if (id >= 0 && static_cast<std::size_t>(id) < cores_.size()) {
            core_data[static_cast<std::size_t>(id)] = data;
        }
    }
    for (std::size_t id = 0; id < cores_.size(); ++id) {
        const auto& data = core_data[id];
        auto& core = cores_[id];
        set_load(*core.load, *core.meter, data.value(QStringLiteral("usagePct")).toDouble(-1),
            !data.isEmpty());
        const auto khz = data.value(QStringLiteral("frequencyKHz")).toInteger();
        if (khz > 0) {
            core.frequency->setText(QString::number(static_cast<double>(khz) / 1e6, 'f', 2));
        } else {
            core.frequency->setText(QString::fromUtf8(kUnavailable));
        }
    }

    const auto memory = snapshot.value(QStringLiteral("memory")).toObject();
    const auto total = memory.value(QStringLiteral("totalBytes")).toInteger();
    const auto used = memory.value(QStringLiteral("usedBytes")).toInteger(-1);
    if (total > 0 && used >= 0 && used <= total) {
        memory_used_->setText(QString::number(static_cast<double>(used) / kGiB, 'f', 2));
        memory_total_->setText(
            QStringLiteral("/ %1 GiB").arg(static_cast<double>(total) / kGiB, 0, 'f', 2));
        set_load(*memory_usage_, *memory_meter_,
            100.0 * static_cast<double>(used) / static_cast<double>(total));
    } else {
        memory_used_->setText(QString::fromUtf8(kUnavailable));
        memory_total_->clear();
        set_load(*memory_usage_, *memory_meter_, -1);
    }

    const auto sensors = snapshot.value(QStringLiteral("temperatureMilliC")).toObject();
    soc_temperature_->setText(temperature(sensors.value(QStringLiteral("soc-thermal"))));
    for (std::size_t id = 0; id < kSensors.size(); ++id) {
        const auto value = sensors.value(QString::fromLatin1(kSensors[id].type));
        temperatures_[id]->setText(temperature(value));
    }

    const auto gpu = snapshot.value(QStringLiteral("gpu")).toObject();
    set_load(*gpu_load_, *gpu_meter_, gpu.value(QStringLiteral("usagePct")).toDouble(-1));
    gpu_frequency_->setText(frequency_mhz(gpu.value(QStringLiteral("frequencyHz"))));
    const auto npu = snapshot.value(QStringLiteral("npu")).toObject();
    npu_frequency_->setText(frequency_mhz(npu.value(QStringLiteral("frequencyHz"))));
    const auto sample_time = snapshot.value(QStringLiteral("sampleTimeMs")).toInteger();
    last_update_ = QDateTime::fromMSecsSinceEpoch(sample_time).toString(QStringLiteral("HH:mm:ss"));
}

auto BoardMonitorPage::showStreamState(MonitorState state, const QString& detail) -> void {
    stream_state_->setToolTip(detail);
    switch (state) {
    case MonitorState::CONNECTING:
        if (last_update_.isEmpty()) {
            stream_state_->setText(QStringLiteral("等待板端采样"));
        } else {
            stream_state_->setText(QStringLiteral("正在连接 · 上次 %1").arg(last_update_));
        }
        break;
    case MonitorState::LIVE:
        stream_state_->setText(QStringLiteral("每秒采样 · %1").arg(last_update_));
        break;
    case MonitorState::STALE:
        if (last_update_.isEmpty()) {
            stream_state_->setText(QStringLiteral("板端监控不可用"));
        } else {
            stream_state_->setText(QStringLiteral("数据过期 · 上次 %1").arg(last_update_));
        }
        break;
    }
}

} // namespace vtb::desktop
