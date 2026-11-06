// Entry point of the drifto game client. All work happens in client::Client:
// Init() creates the window, GL context and network session, Run() executes
// the main loop (fixed-step simulation + rendering) until the window closes,
// and Shutdown() releases resources. This file only parses the command line
// and maps the outcome to a process exit code.
// main.cpp -- drifto client entry point.
#include <cstdio>
#include <string>

#include "client_folder/include/client.h"

int main(int argc, char **argv)
{
    // Parse the command line. On failure print the error (if any) and the usage
    // text, then exit: code 2 for a bad command line, 0 when only help was asked.
    client::Options options;
    std::string error;
    if (!client::parseArgs(argc, argv, options, error))
    {
        if (!error.empty()) std::fprintf(stderr, "drifto: %s\n", error.c_str());
        std::fputs(client::usageText(), error.empty() ? stdout : stderr);
        return error.empty() ? 0 : 2;
    }
    client::Client app(options);
    if (!app.Init()) return 1;
    // Run() returns the exit code; Shutdown() always runs once Init() succeeded.
    int rc = app.Run();
    app.Shutdown();
    return rc;
}
