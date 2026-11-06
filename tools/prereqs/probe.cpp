#include <QApplication>
#include <QDomDocument>

int main(int argc, char** argv)
{
    const QApplication app(argc, argv);
    const QDomDocument xml;
    return xml.isNull() ? 0 : 1;
}
