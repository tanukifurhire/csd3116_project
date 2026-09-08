#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <string>

#include "dds/dds.h"
#include "messages.h"
#include "client.h"

namespace
{
// extern "C" linkage is what signal() expects for the handler function
// pointer; RequestStop() itself only performs an async-signal-safe atomic
// store, so it's safe to call directly here rather than via a self-pipe.
extern "C" void handle_stop_signal(int /*signum*/)
{
    Client::RequestStop();
}
} // namespace

int main(int argc, char *argv[])
{
    // Ctrl+C (SIGINT) or `kill` (SIGTERM) should still let the client tell
    // the server it's leaving and clean up DDS/GL resources, rather than
    // the process dying mid-frame and leaving this player stuck "active" on
    // the server. Installed before anything blocking (the stdin prompts in
    // Init()) so it's caught even during those.
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);

    // Which client cert (certs/clientN.{pem,key}) this instance
    // authenticates as. Defaults to "1" if not given on the command line.
    std::string client_id = "1";
    if (argc > 1)
    {
        client_id = argv[1];

        // Must be a plain positive integer -- it's used to build a
        // certs/clientN.* filename, so anything else is a usage error.
        const bool all_digits = !client_id.empty() &&
            std::all_of(client_id.begin(), client_id.end(),
                        [](unsigned char c) { return std::isdigit(c); });

        if (!all_digits)
        {
            std::cerr << "Usage: " << argv[0] << " [client_id]\n"
                      << "  client_id: positive integer selecting certs/clientN.{pem,key} "
                         "(default 1)\n";
            return 1;
        }
    }

    Client client;
    if (!client.Init(client_id))
    {
        return 1;
    }

    client.Run();
    client.Shutdown();

    return 0;
}
