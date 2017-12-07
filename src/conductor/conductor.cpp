#include <cassert>
#include "config.h"
#include "conductor.h"
#include "passes.h"
#include "chunk/ifunc.h"
#include "elf/elfmap.h"
#include "generate/debugelf.h"
#include "operation/find2.h"
#include "pass/handlerelocs.h"
#include "pass/handledatarelocs.h"
#include "pass/handlecopyrelocs.h"
#include "pass/injectbridge.h"
#include "chunk/serializer.h"
#include "pass/internalcalls.h"
#include "pass/resolveplt.h"
#include "pass/resolvetls.h"
#include "pass/fixjumptables.h"
#include "pass/ifunclazy.h"
#include "pass/fixdataregions.h"
#include "pass/populateplt.h"
#include "pass/relocheck.h"
#include "disasm/objectoriented.h"
#include "transform/data.h"
#include "util/feature.h"
#include "log/log.h"
#include "log/temp.h"

IFuncList *egalito_ifuncList __attribute__((weak));

Conductor::Conductor() {
    forest = new ElfForest();
    program = new Program();
}

Conductor::~Conductor() {
    delete forest;
    delete program;
}

void Conductor::parseExecutable(ElfMap *elf) {
    auto sharedLib = new SharedLib("(executable)", "(executable)", elf);
    getSharedLibList()->addToFront(sharedLib);
    auto module = parse(elf, sharedLib);

    getProgram()->add(module);
    getProgram()->add(new Library(sharedLib->getName(), Library::ROLE_MAIN));
}

void Conductor::parseEgalito(ElfMap *elf) {
    auto library = new SharedLib("(egalito)", "(egalito)", elf);
    getLibraryList()->add(library);
    auto module = parse(elf, library);

    getProgram()->add(module);
    getProgram()->add(new Library(sharedLib->getName(), Library::ROLE_EGALITO));
}

void Conductor::parseLibraries() {
    // we use an index here because the list can change as we iterate
    for(size_t i = 0; i < getSharedLibList()->getCount(); i ++) {
        auto library = getSharedLibList()->get(i);
        auto space = library->getElfSpace();

        if(space) continue;  // already parsed (e.g. libegalito, executable)

        parse(library->getElfMap(), library);
    }
}

Module *Conductor::parseAddOnLibrary(ElfMap *elf) {
    auto library = new SharedLib("(addon)", "(addon)", elf);
    getLibraryList()->add(library);
    auto space = parse(elf, library);
    return space->getModule();
}

Module *Conductor::parse(ElfMap *elf, SharedLib *library) {
    ElfSpace *space = new ElfSpace(elf, library);
    library->setElfSpace(space);

    LOG(1, "\n=== BUILDING ELF DATA STRUCTURES for ["
        << space->getName() << "] ===");
    space->findDependencies(getLibraryList());
    space->findSymbolsAndRelocs();

    LOG(1, "--- RUNNING DEFAULT ELF PASSES for ["
        << space->getName() << "] ---");
    ConductorPasses(this).newElfPasses(space);

    auto module = space->getModule();
    program->add(module);
    return module;
}

void Conductor::parseEgalitoArchive(const char *archive) {
    ChunkSerializer serializer;
    Chunk *newData = serializer.deserialize(archive);

    if(!newData) {
        LOG(1, "Error parsing archive [" << archive << "]");
        return;  // No data present
    }
    else if(auto p = dynamic_cast<Program *>(newData)) {
        LOG(1, "Using full Chunk tree from archive [" << archive << "]");
        this->program = p;
    }
    else {
        LOG(1, "Not using archive, only a subset of the Chunk tree is present");
    }

    ConductorPasses(this).newArchivePasses(program);
}

void Conductor::resolvePLTLinks() {
    ResolvePLTPass resolvePLT(program);
    program->accept(&resolvePLT);

    if(isFeatureEnabled("EGALITO_USE_GS")) {
        PopulatePLTPass populatePLT(this);
        program->accept(&populatePLT);
    }
}

void Conductor::resolveTLSLinks() {
    ResolveTLSPass resolveTLS;
    program->accept(&resolveTLS);
}

void Conductor::resolveWeak() {
    //TemporaryLogLevel tll("conductor", 10);
    //TemporaryLogLevel tll2("chunk", 10);

    for(auto module : CIter::modules(program)) {
        auto space = module->getElfSpace();

        if(module->getName() == "module-(egalito)") {
            InjectBridgePass bridge(space->getRelocList());
            module->accept(&bridge);
        }

        // theoretically this should be three passes, but in practice?
        LOG(10, "[[[1 HandleRelocsWeak]]]" << module->getName());
        HandleRelocsWeak handleRelocsPass(
            space->getElfMap(), space->getRelocList());
        module->accept(&handleRelocsPass);

        LOG(10, "[[[2 HandleDataRelocsExternalStrong]]]" << module->getName());
        HandleDataRelocsExternalStrong pass1(space->getRelocList(), this);
        module->accept(&pass1);

        LOG(10, "[[[3 HandleDataRelocsInternalWeak]]]" << module->getName());
        HandleDataRelocsInternalWeak pass2(space->getRelocList());
        module->accept(&pass2);

        LOG(10, "[[[4 HandleDataRelocsExternalWeak]]]" << module->getName());
        HandleDataRelocsExternalWeak pass3(space->getRelocList(), this);
        module->accept(&pass3);
    }
}

void Conductor::resolveVTables() {
    for(auto module : CIter::modules(program)) {
#ifdef ARCH_X86_64
        // this needs data regions
        module->setVTableList(DisassembleVTables().makeVTableList(
            module->getElfSpace()->getElfMap(),
            module->getElfSpace()->getSymbolList(),
            module->getElfSpace()->getRelocList(), module, program));
#endif
    }
}

void Conductor::setupIFuncLazySelector() {
    this->ifuncList = new IFuncList();
    ::egalito_ifuncList = ifuncList;

    IFuncLazyPass ifuncLazyPass(ifuncList);
    program->accept(&ifuncLazyPass);
}

void Conductor::fixDataSections() {
    // first assign an effective address to each TLS region
    allocateTLSArea();

    HandleCopyRelocs handleCopyRelocs(this);
    program->accept(&handleCopyRelocs);

    fixPointersInData();

    // This has to come after all relocations in TLS are resolved
    loadTLSData();
}

void Conductor::fixPointersInData() {
    FixJumpTablesPass fixJumpTables;
    program->accept(&fixJumpTables);

    FixDataRegionsPass fixDataRegions;
    program->accept(&fixDataRegions);
}

void Conductor::allocateTLSArea() {
    const static address_t base = 0xd0000000;
    DataLoader dataLoader(base);

    // calculate size
    size_t size = 0;
    for(auto module : CIter::modules(program)) {
        auto tls = module->getDataRegionList()->getTLS();
        if(tls) size += tls->getSize();
    }

    if(!size) return;

    // allocate headers
    address_t offset = 0;
    mainThreadPointer = dataLoader.allocateTLS(size, &offset);

    // actually assign address
    for(auto module : CIter::modules(program)) {
        auto tls = module->getDataRegionList()->getTLS();
        if(tls) {
#ifdef ARCH_X86_64
            if(module == program->getMain()) continue;
#endif
            tls->setBaseAddress(base + offset);
            tls->setTLSOffset((base + offset) - mainThreadPointer);
            offset += tls->getSize();
        }
    }

#ifdef ARCH_X86_64
    // x86: place executable's TLS (if present) right before the header
    auto executable = program->getMain();
    if(auto tls = executable->getDataRegionList()->getTLS()) {
        tls->setBaseAddress(base + offset);
        tls->setTLSOffset((base + offset) - mainThreadPointer);
        offset += tls->getSize();
    }
#endif
}

void Conductor::loadTLSData() {
    const static address_t base = 0xd0000000;
    DataLoader dataLoader(base);
    for(auto module : CIter::modules(program)) {
        auto tls = module->getDataRegionList()->getTLS();
        if(tls) {
            dataLoader.loadRegion(module->getElfSpace()->getElfMap(), tls);
        }
    }
}

void Conductor::writeDebugElf(const char *filename, const char *suffix) {
    DebugElf debugElf;

    for(auto module : CIter::modules(program)) {
        for(auto func : CIter::functions(module)) {
            debugElf.add(func, suffix);
        }
    }

    debugElf.writeTo(filename);
}

void Conductor::acceptInAllModules(ChunkVisitor *visitor, bool inEgalito) {
    for(auto module : CIter::modules(program)) {
        if(!inEgalito && module == program->getEgalito()) continue;

        module->accept(visitor);
    }
}

ElfSpace *Conductor::getMainSpace() const {
    return getProgram()->getMain()->getElfSpace();
}

void Conductor::check() {
    ReloCheckPass checker;
    acceptInAllModules(&checker, true);
}
