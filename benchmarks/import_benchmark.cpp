#include "legacy_project.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>

int main(int argc, char** argv)
try
{
    if (argc != 2)
    {
        std::fputs("Usage: motrack_import_benchmark <project.trk>\n", stderr);
        return 1;
    }

    constexpr int ITERATIONS = 100;
    const auto start = std::chrono::steady_clock::now();
    std::size_t marks = 0;
    for (int iteration = 0; iteration < ITERATIONS; ++iteration)
    {
        const auto project = motrack::readLegacyProject(argv[1]);
        for (const auto& track : project.pointMasses)
        {
            marks += track.marks.size();
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    std::cout << ITERATIONS << " imports, " << marks << " marks, " << elapsed.count() / ITERATIONS << " ms per import\n";
    return 0;
}
catch (const std::exception& error)
{
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return 1;
}
