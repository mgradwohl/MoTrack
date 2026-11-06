#include <QApplication>
#include <QDomDocument>

#include <archive.h>

extern "C" {
#include <libavformat/avformat.h>
}

int main(int argc, char** argv)
{
    const QApplication app(argc, argv);
    const QDomDocument xml;
    return xml.isNull() && avformat_version() > 0 && archive_version_number() > 0 ? 0 : 1;
}
