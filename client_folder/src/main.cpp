#include <iostream>
#include <string>

#include "dds/dds.h"
#include "messages.h"
#include "client.h"

int main(int argc, char *argv[])
{
    // Which client cert (certs/clientN.{pem,key}) this instance
    // authenticates as. Defaults to "1" if not given on the command line.
    std::string client_id = "1";
    if (argc > 1)
    {
        client_id = argv[1];
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
