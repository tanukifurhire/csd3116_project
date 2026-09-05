#include <iostream>
#include <string>
#include <thread>

#include "dds/dds.h"
#include "../../messages.h"
#include "../include/client.h"

Client client;

int main()
{
    if (!client.Init())
    {
        return 1;
    }

    client.Run();
    client.Shutdown();
    return 0;
}
