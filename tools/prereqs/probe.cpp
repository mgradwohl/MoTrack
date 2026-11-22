#include <QApplication>
#include <QDomDocument>

#include <version>

#include <archive.h>

extern "C" {
#include <libavformat/avformat.h>
}

// The standard library has to be the one Qt's binaries use: libstdc++ on Linux (GCC 14's, for its
// C++23 library; tools/dev-env.sh selects it) and Apple's libc++ on macOS.
#if defined(__linux__) && (defined(_LIBCPP_VERSION) || !defined(_GLIBCXX_RELEASE) || _GLIBCXX_RELEASE < MOTRACK_GCC_MAJOR)
#error "Linux builds need libstdc++ from GCC MOTRACK_GCC_MAJOR or newer; source tools/dev-env.sh and reconfigure with --fresh."
#endif
#if defined(__APPLE__) && !defined(_LIBCPP_VERSION)
#error "macOS builds need Apple's libc++, as Qt's binaries use; build with Apple Clang."
#endif

int main(int argc, char** argv)
{
    const QApplication app(argc, argv);
    const QDomDocument xml;
    return xml.isNull() && avformat_version() > 0 && archive_version_number() > 0 ? 0 : 1;
}
