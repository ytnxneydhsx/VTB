#include "board_monitor_page.hh"

#include "board_monitor.hh"
#include "monitor_io.hh"
#include "monitor_processes.hh"
#include "monitor_widgets.hh"
#include "theme.hh"
#include "widgets.hh"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>

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

    auto value_label(const char* role = "") -> QLabel* {
        return monitorLabel(QString::fromUtf8(kUnavailable), role);
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
    : QScrollArea(parent)
    , state_(MonitorState::CONNECTING) {
    setObjectName(QStringLiteral("boardMonitorPage"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("page"));
    setWidget(body);
    auto* page = new QVBoxLayout(body);
    page->setContentsMargins(
        metrics::kContentMargin, metrics::kContentMargin, metrics::kContentMargin,
        metrics::kContentMargin);
    page->setSpacing(metrics::kSpacing);

    auto* heading = new QHBoxLayout;
    heading->addWidget(monitorLabel(QStringLiteral("板卡监控"), "title"));
    heading->addStretch();
    stream_state_ = monitorLabel(QStringLiteral("等待板端采样"), "muted");
    stream_state_->setObjectName(QStringLiteral("monitorStreamState"));
    heading->addWidget(stream_state_, 0, Qt::AlignVCenter);
    page->addLayout(heading);

    tabs_ = new QTabBar;
    tabs_->setObjectName(QStringLiteral("monitorTabs"));
    tabs_->setAccessibleName(QStringLiteral("板卡监控页签"));
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    tabs_->addTab(QStringLiteral("概览"));
    const int processes_tab = tabs_->addTab(QStringLiteral("进程"));
    const int io_tab = tabs_->addTab(QStringLiteral("网络与磁盘"));
    auto* tab_row = new QVBoxLayout;
    tab_row->setSpacing(6);
    tab_row->addWidget(tabs_);
    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("monitorDivider"));
    divider->setFixedHeight(1);
    tab_row->addWidget(divider);
    page->addLayout(tab_row);

    auto* sections = new QStackedWidget;
    page->addWidget(sections, 1);
    auto* hardware = new QWidget;
    auto* content = new QVBoxLayout(hardware);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(metrics::kSpacing);
    sections->addWidget(hardware);
    processes_ = new MonitorProcesses;
    io_ = new MonitorIo;
    sections->addWidget(processes_);
    sections->addWidget(io_);

    connect(tabs_, &QTabBar::currentChanged, this, [this, sections](int index) {
        sections->setCurrentIndex(index);
        verticalScrollBar()->setValue(0);
        updateStreamLabel();
    });

    auto* overview = new QHBoxLayout;
    overview->setSpacing(metrics::kSpacing);
    content->addLayout(overview);

    auto* cpu = monitorPanel(*overview, {});
    cpu->addWidget(monitorLabel(QStringLiteral("CPU 总占用"), "sub"));
    cpu_total_ = value_label("monitorValue");
    cpu_total_->setObjectName(QStringLiteral("monitorCpuTotal"));
    cpu->addWidget(cpu_total_);
    cpu_meter_ = meter(QStringLiteral("CPU 总占用"));
    cpu->addWidget(cpu_meter_);

    auto* ram = monitorPanel(*overview, {});
    ram->addWidget(monitorLabel(QStringLiteral("RAM"), "sub"));
    auto* memory_values = new QHBoxLayout;
    memory_values->setSpacing(6);
    memory_used_ = value_label("monitorValue");
    memory_total_ = monitorLabel({}, "muted");
    memory_usage_ = value_label("muted");
    memory_values->addWidget(memory_used_, 0, Qt::AlignBaseline);
    memory_values->addWidget(memory_total_, 0, Qt::AlignBaseline);
    memory_values->addStretch();
    memory_values->addWidget(memory_usage_, 0, Qt::AlignBaseline);
    ram->addLayout(memory_values);
    memory_meter_ = meter(QStringLiteral("RAM 使用率"));
    ram->addWidget(memory_meter_);

    auto* soc = monitorPanel(*overview, {});
    soc->addWidget(monitorLabel(QStringLiteral("SoC 温度"), "sub"));
    soc_temperature_ = value_label("monitorValue");
    soc->addWidget(soc_temperature_);
    soc->addWidget(monitorLabel(QStringLiteral("soc-thermal"), "muted"));

    auto* details = new QHBoxLayout;
    details->setSpacing(metrics::kSpacing);
    auto* left = new QVBoxLayout;
    auto* right = new QVBoxLayout;
    right->setSpacing(metrics::kSpacing);
    details->addLayout(left, 6);
    details->addLayout(right, 5);
    content->addLayout(details);

    auto* cpu_details = monitorPanel(*left, {});
    auto* cpu_title = new QHBoxLayout;
    cpu_title->addWidget(monitorLabel(QStringLiteral("CPU 逐核"), "heading"));
    cpu_title->addStretch();
    cpu_title->addWidget(monitorLabel(QStringLiteral("8 核"), "muted"));
    cpu_details->addLayout(cpu_title);

    auto* cpu_table = new QGridLayout;
    cpu_table->setVerticalSpacing(3);
    cpu_table->setHorizontalSpacing(9);
    cpu_table->setColumnStretch(1, 1);
    cpu_table->addWidget(monitorLabel(QStringLiteral("核心"), "muted"), 0, 0);
    cpu_table->addWidget(monitorLabel(QStringLiteral("占用"), "muted"), 0, 1);
    cpu_table->addWidget(monitorLabel(QStringLiteral("频率（GHz）"), "muted"), 0, 3, Qt::AlignRight);
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
        auto* name_widget = monitorLabel(name, "sub");
        name_widget->setMinimumHeight(22);
        cpu_table->addWidget(name_widget, row + 1, 0);
        cpu_table->addWidget(core.meter, row + 1, 1, Qt::AlignVCenter);
        cpu_table->addWidget(core.load, row + 1, 2);
        cpu_table->addWidget(core.frequency, row + 1, 3, Qt::AlignRight);
    }
    cpu_details->addLayout(cpu_table);
    left->addStretch();

    auto* gpu = monitorPanel(*right, {});
    auto* gpu_top = new QHBoxLayout;
    gpu_top->addWidget(monitorLabel(QStringLiteral("GPU"), "heading"));
    gpu_top->addStretch();
    gpu_frequency_ = value_label("monitorFrequency");
    gpu_top->addWidget(gpu_frequency_);
    gpu->addLayout(gpu_top);
    auto* gpu_bottom = new QHBoxLayout;
    gpu_bottom->setSpacing(12);
    gpu_bottom->addWidget(monitorLabel(QStringLiteral("负载"), "sub"));
    gpu_meter_ = meter(QStringLiteral("GPU 负载"));
    gpu_bottom->addWidget(gpu_meter_, 1, Qt::AlignVCenter);
    gpu_load_ = value_label("sub");
    gpu_load_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gpu_load_->setMinimumWidth(36);
    gpu_bottom->addWidget(gpu_load_);
    gpu->addLayout(gpu_bottom);

    auto* npu = monitorPanel(*right, {});
    auto* npu_top = new QHBoxLayout;
    npu_top->addWidget(monitorLabel(QStringLiteral("NPU"), "heading"));
    npu_top->addStretch();
    npu_frequency_ = value_label("monitorFrequency");
    npu_top->addWidget(npu_frequency_);
    npu->addLayout(npu_top);
    npu->addWidget(monitorLabel(QStringLiteral("当前频率"), "muted"));

    auto* temperature_details = monitorPanel(*right, QStringLiteral("温度传感器"));
    auto* sensor_grid = new QGridLayout;
    sensor_grid->setHorizontalSpacing(8);
    sensor_grid->setVerticalSpacing(3);
    for (std::size_t id = 0; id < kSensors.size(); ++id) {
        const auto col = static_cast<int>(id % 3);
        const auto row = static_cast<int>(id / 3);
        auto* name = monitorLabel(QString::fromUtf8(kSensors[id].label), "muted");
        temperatures_[id] = value_label("sub");
        sensor_grid->addWidget(name, row * 2, col);
        sensor_grid->addWidget(temperatures_[id], row * 2 + 1, col);
        sensor_grid->setColumnStretch(col, 1);
    }
    temperature_details->addLayout(sensor_grid);
    right->addStretch();

    auto* summaries = new QHBoxLayout;
    summaries->setSpacing(metrics::kSpacing);
    content->addLayout(summaries);
    auto* open_processes = new ActionButton({QStringLiteral("查看进程"), ButtonKind::TONAL});
    auto* open_io = new ActionButton({QStringLiteral("查看速率"), ButtonKind::TONAL});
    open_processes->setObjectName(QStringLiteral("monitorJump"));
    open_io->setObjectName(QStringLiteral("monitorJump"));
    processes_->addOverview(*summaries, open_processes);
    io_->addOverview(*summaries, open_io);
    connect(open_processes, &QPushButton::clicked, this,
        [this, processes_tab] { tabs_->setCurrentIndex(processes_tab); });
    connect(open_io, &QPushButton::clicked, this, [this, io_tab] { tabs_->setCurrentIndex(io_tab); });
    content->addStretch();
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
    details_supported_ = snapshot.value(QStringLiteral("details")).toBool();
    if (snapshot.value(QStringLiteral("io")).isObject()) {
        io_->showSample(snapshot.value(QStringLiteral("io")).toObject());
        last_details_update_ = last_update_;
    }
}

auto BoardMonitorPage::showProcesses(const QJsonArray& rows, bool readable) -> void {
    processes_->showProcesses(rows, readable);
}

auto BoardMonitorPage::showServiceStatus(const BoardStatus& status) -> void {
    processes_->showStatus(status);
}

auto BoardMonitorPage::showStreamState(MonitorState state, const QString& detail) -> void {
    state_ = state;
    stream_state_->setToolTip(detail);
    const bool stale = state == MonitorState::STALE;
    processes_->showAvailability(details_supported_, stale);
    io_->showAvailability(details_supported_, stale);
    updateStreamLabel();
}

auto BoardMonitorPage::updateStreamLabel() -> void {
    const bool overview = tabs_->currentIndex() == 0;
    auto last = last_details_update_;
    if (overview) {
        last = last_update_;
    }
    switch (state_) {
    case MonitorState::CONNECTING:
        if (last.isEmpty()) {
            stream_state_->setText(QStringLiteral("等待板端采样"));
        } else {
            stream_state_->setText(QStringLiteral("正在连接 · 上次 %1").arg(last));
        }
        break;
    case MonitorState::LIVE:
        if (overview) {
            stream_state_->setText(QStringLiteral("每秒采样 · %1").arg(last));
        } else if (!details_supported_) {
            stream_state_->setText(QStringLiteral("需更新板端采集器"));
        } else if (last.isEmpty()) {
            stream_state_->setText(QStringLiteral("等待板端采样"));
        } else {
            stream_state_->setText(QStringLiteral("每 2 秒更新 · %1").arg(last));
        }
        break;
    case MonitorState::STALE:
        if (last.isEmpty()) {
            stream_state_->setText(QStringLiteral("板端监控不可用"));
        } else {
            stream_state_->setText(QStringLiteral("数据过期 · 上次 %1").arg(last));
        }
        break;
    }
}

} // namespace vtb::desktop
