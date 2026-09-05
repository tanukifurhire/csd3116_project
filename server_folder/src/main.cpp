#include <iostream>
#include <string>
#include <thread>

#include "dds/dds.h"
#include "../../messages.h"
#include "../include/server.h"

Server server;

int main()
{
    server.Init();
    return 0;
}