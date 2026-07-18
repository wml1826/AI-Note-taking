QT += core gui widgets network sql
CONFIG += c++17
TARGET = ai_chat_desktop
TEMPLATE = app
SOURCES += main.cpp chatclient.cpp notesclient.cpp ragclient.cpp dbmanager.cpp mainwindow.cpp
HEADERS += types.h chatclient.h notesclient.h ragclient.h dbmanager.h mainwindow.h
FORMS += mainwindow.ui imageanalysispage.ui settingspage.ui
win32:LIBS += -lpsapi

win32 {
    CONFIG(debug, debug|release) {
        OPENSSL_DEST = $$OUT_PWD/debug
        DEPLOY_MODE = --debug
    } else {
        OPENSSL_DEST = $$OUT_PWD/release
        DEPLOY_MODE = --release
    }

    WINDEPLOYQT = $$shell_path($$[QT_INSTALL_BINS]/windeployqt.exe)

    # Copy OpenSSL and deploy the matching Qt/MinGW runtime and plugins.
    QMAKE_POST_LINK = $$QMAKE_COPY \"$$shell_path($$PWD/libssl-1_1-x64.dll)\" \"$$shell_path($$OPENSSL_DEST)\" $$escape_expand(\n\t) \
                      $$QMAKE_COPY \"$$shell_path($$PWD/libcrypto-1_1-x64.dll)\" \"$$shell_path($$OPENSSL_DEST)\" $$escape_expand(\n\t) \
                      $$WINDEPLOYQT $$DEPLOY_MODE --compiler-runtime --no-translations \
                      \"$$shell_path($$OPENSSL_DEST/ai_chat_desktop.exe)\"
}
