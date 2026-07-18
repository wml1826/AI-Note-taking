#include "mainwindow.h"
#include <QApplication>
#include <QSslSocket>
#include <QDebug>

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    QApplication::setApplicationName("AI Chat Desktop");

    // SSL/TLS 诊断：启动时立即打印 OpenSSL 状态
    qDebug() << "=== SSL Diagnostic ===";
    qDebug() << "supportsSsl:" << QSslSocket::supportsSsl();
    qDebug() << "sslLibraryBuildVersion:" << QSslSocket::sslLibraryBuildVersionString();
    qDebug() << "sslLibraryRuntimeVersion:" << QSslSocket::sslLibraryVersionString();
    qDebug() << "======================";

    MainWindow w;
    w.show();
    return a.exec();
}
