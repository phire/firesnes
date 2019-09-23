#include <iostream>
#include "m65816.h"

int main(int, char**) {
    m65816 core;

    core.run_for(100);
}
