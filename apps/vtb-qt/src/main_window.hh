#pragma once

#include <QMainWindow>

#include <memory>

namespace vtb::desktop {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool demo);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    auto operator=(const MainWindow&) -> MainWindow& = delete;
    MainWindow(MainWindow&&) = delete;
    auto operator=(MainWindow&&) -> MainWindow& = delete;

protected:
    auto closeEvent(QCloseEvent* event) -> void override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtb::desktop
