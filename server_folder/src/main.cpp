#include <iostream>

#include "dds/dds.h"
#include "messages.h"
#include "server.h"

int main()
{
    Server server;

    if (!server.Init())
    {
        return 1;
    }

    server.Run();
    server.Shutdown();

    return 0;
}
