#include "main_window.hpp"
#include "support.hpp"

#include <digzip/version.hpp>

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("DigZip"));
    QApplication::setApplicationVersion(QString::fromStdString(digzip::version_info().program));
    digzip::gui::install_translations(app);

    digzip::gui::MainWindow window;
    window.show();

    // "digzip-gui archive.zip", also what a file association or a drop on the program runs.
    const QStringList arguments = QApplication::arguments();
    if (arguments.size() > 1) {
        window.open_archive(arguments.at(1));
    }
    return QApplication::exec();
}
