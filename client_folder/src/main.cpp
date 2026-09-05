#include <iostream>
#include <string>
#include <thread>

#include "dds/dds.h"
#include "../../messages.h"
#include "../include/client.h"

Client client;

int main()
{
    client.Init();
    return 0;
}