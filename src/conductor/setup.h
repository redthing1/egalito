#ifndef EGALITO_CONDUCTOR_SETUP_H
#define EGALITO_CONDUCTOR_SETUP_H

#include "elf/elfmap.h"
#include "elf/elfspace.h"

class Conductor;
class Sandbox;

/** Main setup class for Egalito.

    Note: please call functions in the order they appear, i.e.
        parseElfFiles(),
        makeLoaderSandbox() / makeFileSandbox(),
        moveCode() OR its three components
            moveCodeAssignAddresses(),
            copyCodeToNewAddresses(),
            moveCodeMakeExecutable()
*/
class ConductorSetup {
private:
    ElfMap *elf;
    ElfMap *egalito;
    Conductor *conductor;
    Sandbox *sandbox;
public:
    ConductorSetup() : elf(nullptr), egalito(nullptr), conductor(nullptr),
        sandbox(nullptr) {}
    void parseElfFiles(const char *executable, bool withSharedLibs = true,
        bool injectEgalito = false);
    void makeLoaderSandbox();
    void makeFileSandbox(const char *outputFile);
    void moveCode();
public:
    void moveCodeAssignAddresses();
    void copyCodeToNewAddresses();
    void moveCodeMakeExecutable();
public:
    ElfMap *getElfMap() const { return elf; }
    ElfMap *getEgalitoElfMap() const { return egalito; }
    Conductor *getConductor() const { return conductor; }
    Sandbox *getSandbox() const { return sandbox; }
public:
    void dumpElfSpace(ElfSpace *space);
    void dumpFunction(const char *function, ElfSpace *space = nullptr);
    address_t getEntryPoint();
    void *getMainThreadPointer();
private:
    bool setBaseAddress(ElfMap *map, address_t base);
};

#endif