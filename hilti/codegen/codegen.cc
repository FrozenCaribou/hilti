
#include <util/util.h>

#include "../module.h"
#include "../options.h"
#include "../statement.h"

#include "codegen.h"
#include "util.h"
#include "loader.h"
#include "storer.h"
#include "unpacker.h"
#include "packer.h"
#include "field-builder.h"
#include "coercer.h"
#include "stmt-builder.h"
#include "type-builder.h"
#include "abi.h"
#include "debug-info-builder.h"
#include "../passes/collector.h"
#include "../builder/nodes.h"

#include "libhilti/enum.h"

using namespace hilti;
using namespace codegen;

CodeGen::CodeGen(CompilerContext* ctx, const path_list& libdirs)
    : _loader(new Loader(this)),
      _storer(new Storer(this)),
      _unpacker(new Unpacker(this)),
      _packer(new Packer(this)),
      _field_builder(new FieldBuilder(this)),
      _stmt_builder(new StatementBuilder(this)),
      _coercer(new Coercer(this)),
      _type_builder(new TypeBuilder(this)),
      _debug_info_builder(new DebugInfoBuilder(this)),
      _collector(new passes::Collector())
{
    _ctx = ctx;
    _libdirs = libdirs;
    setLoggerName("codegen");
}

CodeGen::~CodeGen()
{}

CompilerContext* CodeGen::context() const
{
    return _ctx;
}

const Options& CodeGen::options() const
{
    return _ctx->options();
}

llvm::Module* CodeGen::generateLLVM(shared_ptr<hilti::Module> hltmod)
{
    _hilti_module = hltmod;
    _functions.clear();

    if ( options().cgDebugging("codegen") )
        debugSetLevel(1);

    if ( ! _collector->run(hltmod) )
        return nullptr;

    try {
        if ( ! _libhilti ) {
            string libhilti = ::util::findInPaths("libhilti.ll", _libdirs);

            if ( libhilti.size() == 0 )
                fatalError("cannot find libhilti.ll in library search path");

            llvm::SMDiagnostic diag;

            _libhilti = llvm::ParseAssemblyFile(libhilti, diag, llvmContext());

            if ( ! _libhilti )
                fatalError(::util::fmt("cannot load libhilti.ll: %s (\"%s\")", diag.getMessage().str(), diag.getLineContents().str()));
        }

        _abi = std::move(ABI::createABI(this));

        _module = new ::llvm::Module(util::mangle(hltmod->id(), false), llvmContext());
        _module->setTargetTriple(_abi->targetTriple());
        _module->setDataLayout(_abi->dataLayout());

        auto name = llvm::MDString::get(llvmContext(), linkerModuleIdentifier());
        auto md = _module->getOrInsertNamedMetadata(symbols::MetaModuleName);
        md->addOperand(codegen::util::llvmMdFromValue(llvmContext(), name));

        _data_layout = new ::llvm::DataLayout(_abi->dataLayout());

        createInitFunction();

        initGlobals();

        // Kick-off recursive code generation.
        _stmt_builder->llvmStatement(hltmod->body());

        finishInitFunction();

        createGlobalsInitFunction();

        createLinkerData();

        createRtti();

        _type_builder->finalize();

        return _module;
    }

    catch ( const ast::FatalLoggerError& err ) {
        // Message has already been printed.
        return nullptr;
    }

}

void CodeGen::llvmInsertComment(const string& comment)
{
    _functions.back()->next_comment = comment;
}

llvm::Value* CodeGen::llvmCoerceTo(llvm::Value* value, shared_ptr<hilti::Type> src, shared_ptr<hilti::Type> dst, bool cctor)
{
    return _coercer->llvmCoerceTo(value, src, dst, cctor);
}

llvm::Type* CodeGen::llvmLibType(const string& name)
{
    auto type = lookupCachedType("libhilti", name);

    if ( type )
        return type;

    type = _libhilti->getTypeByName(name);

    if ( ! type )
        internalError(::util::fmt("type %s not found in libhilti.ll", name.c_str()));

    // We need to recreate the type as otherwise the linker gets messed up
    // when we reuse the same library value directly (and in separate
    // modules).

    auto stype = llvm::cast<llvm::StructType>(type);

    std::vector<llvm::Type*> fields;
    for ( auto i = stype->element_begin(); i != stype->element_end(); ++i )
        fields.push_back(*i);

    type = llvm::StructType::create(llvmContext(), fields, string(name));
    return cacheType("libhilti", name, type);
}

llvm::Type* CodeGen::replaceLibType(llvm::Type* ntype)
{
    auto t = ntype;
    int depth = 0;

    while ( true ) {
        auto ptype = llvm::dyn_cast<llvm::PointerType>(t);
        if ( ! ptype )
            break;

        t = ptype->getElementType();
        ++depth;
    }

    auto stype = llvm::dyn_cast<llvm::StructType>(t);

    if ( stype ) {
        auto name = stype->getName().str();

        if ( name.size() ) {
            auto i = name.rfind(".");
            if ( i != string::npos && isdigit(name[i+1]) )
                name = name.substr(0, i);
        }

        if ( _libhilti->getTypeByName(name) ) {
            ntype = llvmLibType(name.c_str());

            while ( depth-- )
                ntype = llvm::PointerType::get(ntype, 0);
        }
    }

    return ntype;
}

llvm::Function* CodeGen::llvmLibFunction(const string& name)
{
    llvm::Value* val = lookupCachedValue("function", name);
    if ( val )
        return llvm::cast<llvm::Function>(val);

    auto func = _libhilti->getFunction(name);
    if ( ! func )
        internalError(::util::fmt("function %s not found in libhilti.ll", name.c_str()));

    // As we recreate the library types in llvmLibType, they now won't match
    // anymore what function prototype specify. So we need to recreate the
    // function as well. Sigh.

    std::vector<llvm::Type *> args;

    for ( auto arg = func->arg_begin(); arg != func->arg_end(); ++arg ) {
        args.push_back(replaceLibType(arg->getType()));
    }

    auto rtype = replaceLibType(func->getReturnType());
    auto ftype = llvm::FunctionType::get(rtype, args, false);
    auto nfunc = llvm::Function::Create(ftype, func->getLinkage(), func->getName(), _module);

    cacheValue("function", name, nfunc);
    return nfunc;
}

llvm::GlobalVariable* CodeGen::llvmLibGlobal(const string& name)
{
    auto glob = _libhilti->getGlobalVariable(name, true);
    if ( ! glob )
        internalError(::util::fmt("global %s not found in libhilti.ll", name.c_str()));

    return glob;
}

llvm::Value* CodeGen::llvmLocal(const string& name)
{
    auto map = _functions.back()->locals;

    auto i = map.find(name);

    if ( i == map.end() ) {
        for ( auto l : map )
            fprintf(stderr, "| %s\n", l.first.c_str());

        internalError("unknown local " + name);
    }

    return std::get<0>(i->second);
}

llvm::Value* CodeGen::llvmGlobal(shared_ptr<Variable> var)
{
    return llvmGlobal(var.get());
}

string CodeGen::scopedNameGlobal(Variable* var) const
{
    auto scope = var->id()->scope();

    if ( scope.empty() )
        scope = _hilti_module->id()->name();

    return ::util::fmt("%s::%s", scope, var->id()->local());
}

llvm::Value* CodeGen::llvmGlobal(Variable* var)
{

    // The linker will replace th=is code with the actual global value.
    llvm::Value* dummy = builder()->CreateAlloca(llvmTypePtr(llvmType(var->type())));
    auto ins = builder()->CreateLoad(dummy);

    auto mdglobal = llvm::MDString::get(llvmContext(), scopedNameGlobal(var));
    std::vector<llvm::Value*> vals = { mdglobal };
    auto md = llvm::MDNode::get(llvmContext(), vals);
    llvm::cast<llvm::Instruction>(ins)->setMetadata("global-access", md);

    return ins;
}


llvm::Value* CodeGen::llvmValue(shared_ptr<Expression> expr, shared_ptr<hilti::Type> coerce_to, bool cctor)
{
    return _loader->llvmValue(expr, cctor, coerce_to);
}

void CodeGen::llvmValueInto(llvm::Value* dst, shared_ptr<Expression> expr, shared_ptr<hilti::Type> coerce_to, bool cctor)
{
    _loader->llvmValueInto(dst, expr, cctor, coerce_to);
}

llvm::Value* CodeGen::llvmValueAddress(shared_ptr<Expression> expr)
{
    return _loader->llvmValueAddress(expr);
}

#if 0
llvm::Value* CodeGen::llvmValue(shared_ptr<Constant> constant)
{
    return _loader->llvmValue(constant);
}
#endif

llvm::Value* CodeGen::llvmExecutionContext()
{
    auto ctx = _functions.back()->context;

    if ( ctx )
        return ctx;

    for ( auto arg = function()->arg_begin(); arg != function()->arg_end(); ++arg ) {
        if ( arg->getName() == symbols::ArgExecutionContext )
            return arg;
    }

    internalError("no context argument found in llvmExecutionContext");
    return 0;
}

llvm::Value* CodeGen::llvmThreadMgr()
{
    return llvmCallC("hlt_global_thread_mgr", {}, false, false);
}

llvm::Value* CodeGen::llvmGlobalExecutionContext()
{
    return llvmCallC("hlt_global_execution_context", {}, false, false);
}

llvm::Constant* CodeGen::llvmSizeOf(llvm::Constant* v)
{
    return llvmSizeOf(v->getType());
}

uint64_t CodeGen::llvmSizeOfForTarget(llvm::Type* t)
{
    return _data_layout->getTypeAllocSize(t);
}

llvm::Constant* CodeGen::llvmSizeOf(llvm::Type* t)
{
    // Computer size using the "portable sizeof" idiom ...
    auto null = llvmConstNull(llvmTypePtr(t));
    auto addr = llvm::ConstantExpr::getGetElementPtr(null, llvmGEPIdx(1));
    return llvm::ConstantExpr::getPtrToInt(addr, llvmTypeInt(64));
}

void CodeGen::llvmStore(shared_ptr<hilti::Expression> target, llvm::Value* value, bool dtor_first)
{
    _storer->llvmStore(target, value, false, dtor_first);
}

std::pair<llvm::Value*, llvm::Value*> CodeGen::llvmUnpack(
                   shared_ptr<Type> type, shared_ptr<Expression> begin,
                   shared_ptr<Expression> end, shared_ptr<Expression> fmt,
                   shared_ptr<Expression> arg,
                   const Location& location)
{
    UnpackArgs args;
    args.type = type;
    args.begin = begin ? llvmValue(begin) : nullptr;
    args.end = end ? llvmValue(end) : nullptr;
    args.fmt = fmt ? llvmValue(fmt) : nullptr;
    args.arg = arg ? llvmValue(arg) : nullptr;
    args.arg_type = arg ? arg->type() : nullptr;
    args.location = location;

    UnpackResult result = _unpacker->llvmUnpack(args);

    auto val = builder()->CreateLoad(result.value_ptr);
    auto iter = builder()->CreateLoad(result.iter_ptr);

    return std::make_pair(val, iter);
}

std::pair<llvm::Value*, llvm::Value*> CodeGen::llvmUnpack(
                   shared_ptr<Type> type, llvm::Value* begin,
                   llvm::Value* end, llvm::Value* fmt,
                   llvm::Value* arg, shared_ptr<Type> arg_type,
                   const Location& location)
{
    UnpackArgs args;
    args.type = type;
    args.begin = begin;
    args.end = end;
    args.fmt = fmt;
    args.arg = arg;
    args.arg_type = arg_type;
    args.location = location;

    UnpackResult result = _unpacker->llvmUnpack(args);

    auto val = builder()->CreateLoad(result.value_ptr);
    auto iter = builder()->CreateLoad(result.iter_ptr);

    return std::make_pair(val, iter);
}

llvm::Value* CodeGen::llvmPack(shared_ptr<Expression> value,
                               shared_ptr<Expression> fmt, shared_ptr<Expression> arg,
                               const Location& location)
{
    PackArgs args;
    args.value = value ? llvmValue(value) : nullptr;
    args.type = value ? value->type() : nullptr;
    args.fmt = fmt ? llvmValue(fmt) : nullptr;
    args.arg = arg ? llvmValue(arg) : nullptr;
    args.arg_type = arg ? arg->type() : nullptr;
    args.location = location;

    return _packer->llvmPack(args);
}

llvm::Value* CodeGen::llvmPack(llvm::Value* value, shared_ptr<Type> type, llvm::Value* fmt,
                              llvm::Value* arg, shared_ptr<Type> arg_type,
                              const Location& location)
{
    PackArgs args;
    args.value = value;
    args.type = type;
    args.fmt = fmt;
    args.arg = arg;
    args.arg_type = arg_type;
    args.location = location;

    return _packer->llvmPack(args);
}


llvm::Value* CodeGen::llvmParameter(shared_ptr<type::function::Parameter> param)
{
    auto name = param->id()->name();
    auto func = function();

    llvm::Value* val = nullptr;

    for ( auto arg = func->arg_begin(); arg != func->arg_end(); arg++ ) {
        if ( arg->getName() == name ) {
            if ( arg->hasByValAttr() )
                val = builder()->CreateLoad(arg);
            else
                val = arg;

            // Reinterpret to account for potential ABI mangling.
            auto ltype = llvmType(param->type());
            return llvmReinterpret(val, ltype);
        }
    }

    internalError("unknown parameter name " + name);
    return 0; // Never reached.
}

void CodeGen::llvmStore(statement::Instruction* instr, llvm::Value* value, bool dtor_first)
{
    _storer->llvmStore(instr->target(), value, false, dtor_first);
}

llvm::Function* CodeGen::pushFunction(llvm::Function* function, bool push_builder, bool abort_on_excpt, bool is_init_func, type::function::CallingConvention cc)
{
    unique_ptr<FunctionState> state(new FunctionState);
    state->function = function;
    state->dtors_after_call = false;
    state->abort_on_excpt = abort_on_excpt;
    state->is_init_func = is_init_func;
    state->context = nullptr;
    state->cc = cc;
    _functions.push_back(std::move(state));

    if ( push_builder )
        pushBuilder("entry");

    return function;
}

void CodeGen::popFunction()
{
    if ( ! block()->getTerminator() )
        // Add a return if we don't have one yet.
        llvmReturn();

    llvmBuildExitBlock();
    llvmNormalizeBlocks();

    _functions.pop_back();
}

llvm::BasicBlock* CodeGen::block() const
{
    return builder()->GetInsertBlock();
}

bool CodeGen::functionEmpty() const
{
    auto func = function();
    auto size = function()->getBasicBlockList().size();

    if ( size == 0 || (size == 1 && func->getEntryBlock().empty()) )
        return true;

    return false;
}

IRBuilder* CodeGen::builder() const
{
    assert(_functions.size());
    return _functions.back()->builders.back();
}

IRBuilder* CodeGen::pushBuilder(IRBuilder* builder)
{
    assert(_functions.size());
    _functions.back()->builders.push_back(builder);
    return builder;
}

IRBuilder* CodeGen::pushBuilder(string name, bool reuse)
{
    return pushBuilder(newBuilder(name, reuse));
}

IRBuilder* CodeGen::newBuilder(string name, bool reuse, bool create)
{
    int cnt = 1;

    name = util::mangle(name, false);

    string n;

    while ( true ) {
        if ( cnt == 1 )
            n = ::util::fmt(".%s", name.c_str());
        else
            n = ::util::fmt(".%s.%d", name.c_str(), cnt);

        auto b = _functions.back()->builders_by_name.find(n);

        if ( b == _functions.back()->builders_by_name.end() )
            break;

        if ( reuse )
            return b->second;

        ++cnt;
    }

    if ( ! create )
    return 0;

    auto block = llvm::BasicBlock::Create(llvmContext(), n, function());
    auto builder = newBuilder(block);

    _functions.back()->builders_by_name.insert(std::make_pair(n, builder));

    return builder;
}

IRBuilder* CodeGen::newBuilder(llvm::BasicBlock* block, bool insert_at_beginning)
{
    return util::newBuilder(this, block, insert_at_beginning);
}

string CodeGen::mangleGlobal(shared_ptr<ID> id, shared_ptr<Module> mod, string prefix, bool internal)
{
    string m = "";

    if ( id->isScoped() ) {
        auto p = id->path();
        auto mod = p.front();
        p.pop_front();
        m = ::util::fmt("%s::%s", ::util::strtolower(mod), ::util::strjoin(p, "::"));
    }

    else {
        if ( ! mod )
            mod = id->firstParent<Module>();

        if ( mod )
            m = ::util::fmt("%s::%s", ::util::strtolower(mod->id()->name()), id->name());
        else
            m = id->name();
    }

    return util::mangle(std::make_shared<ID>(m), true, nullptr, prefix, internal);
}

IRBuilder* CodeGen::builderForLabel(const string& name)
{
    return newBuilder(name, true);
}

IRBuilder* CodeGen::haveBuilder(const string& name)
{
    return newBuilder(name, true, false);
}

void CodeGen::popBuilder()
{
    assert(_functions.size());
    _functions.back()->builders.pop_back();
}

llvm::Function* CodeGen::llvmPushLinkerJoinableFunction(const string& name)
{
    // We use a void pointer here for the execution context to avoid type
    // trouble a link time when merging modules.
    //
    // Also, the linker can in principle deal with more and other arguments
    // as well, the joined function will have the same signature as the one
    // we create here. However, when using custom types, things can get
    // messed up if the same function is also declared in libhilti.

    llvm_parameter_list params;
    params.push_back(std::make_pair(symbols::ArgExecutionContext, llvmTypePtr())); // Name must match w/ linker.

    auto func = llvmAddFunction(name, llvmTypeVoid(), params, false, type::function::C);
    pushFunction(func, true, false, true);

    auto arg1 = func->arg_begin();
    auto ctx = builder()->CreateBitCast(arg1, llvmTypePtr(llvmTypeExecutionContext()));
    _functions.back()->context = ctx;
    _functions.back()->abort_on_excpt = true;

    return func;
}

void CodeGen::createInitFunction()
{
    string name = util::mangle(_hilti_module->id(), true, nullptr, ::util::fmt("init.module.%s", linkerModuleIdentifier()));
    _module_init_func = llvmPushLinkerJoinableFunction(name);
}

llvm::Function* CodeGen::llvmModuleInitFunction()
{
    return _module_init_func;
}

void CodeGen::finishInitFunction()
{
    // Make sure the function stack has been popped back to the original
    // state.
    assert(function() == _module_init_func);

    if ( ! functionEmpty() ) {
#if 0
        // Add a terminator to the function.
        builder()->CreateRetVoid();
#endif
    }

    else {
        // We haven't added anything to the function, just discard.
        _module_init_func->removeFromParent();
        _module_init_func = nullptr;
    }

    // Pop it.
    popFunction();
}

void CodeGen::initGlobals()
{
    // Create the %hlt.globals.type struct with all our global variables.
    std::vector<llvm::Type*> globals;

    for ( auto g : _collector->globals() ) {
        auto t = llvmType(g->type());
        _globals.insert(make_pair(g.get(), llvmGEPIdx(globals.size())));
        globals.push_back(t);
    }

    // This global will be accessed by our custom linker. Unfortunastely, it
    // seems, we can't store a type in a metadata node directly, which would
    // simplify the linker.
    string postfix = string(".") + linkerModuleIdentifier();
    _globals_type = llvmTypeStruct(symbols::TypeGlobals + postfix, globals);

    if ( globals.size() ) {
        // Create the @hlt.globals.base() function. This will be called when we
        // need the base address for our globals, but all calls will later be replaced by the linker.
        string name = symbols::FuncGlobalsBase + postfix;

        CodeGen::parameter_list no_params;
        _globals_base_func = llvmAddFunction(name, llvmTypePtr(_globals_type), no_params, false, type::function::C); // C to not mess with parameters.
    }
}

void CodeGen::createGlobalsInitFunction()
{
    // If we don't have any globals, we don't need any of the following.
    if ( _collector->globals().size() == 0 && _global_strings.size() == 0 )
        return;

    string postfix = string(".") + _hilti_module->id()->name();

    // Create a function that initializes our globals with defaults.
    auto name = util::mangle(_hilti_module->id(), true, nullptr, ::util::fmt("init.globals.%s", linkerModuleIdentifier()));
    _globals_init_func = llvmPushLinkerJoinableFunction(name);

    // Init user defined globals.
    for ( auto g : _collector->globals() ) {
        llvmDebugPrint("hilti-trace", ::util::fmt("init global %s", g->id()->pathAsString()));
        auto init = g->init() ? llvmValue(g->init(), g->type(), true) : llvmInitVal(g->type());
        assert(init);
        auto addr = llvmGlobal(g.get());
        llvmCreateStore(init, addr);
        llvmBuildInstructionCleanup();
        llvmCheckException();
    }

    if ( functionEmpty() ) {
        // We haven't added anything to the function, just discard.
        _globals_init_func->removeFromParent();
        _globals_init_func = nullptr;
    }

    popFunction();

    // Create a function that that destroys all the memory managed objects in
    // there.
    name = util::mangle(_hilti_module->id(), true, nullptr, ::util::fmt("dtor.globals.%s", linkerModuleIdentifier()));
    _globals_dtor_func = llvmPushLinkerJoinableFunction(name);

    for ( auto g : _collector->globals() ) {
        auto val = llvmGlobal(g);
        llvmDtor(val, g->type(), true, "init-globals");
    }

    auto stype = shared_ptr<Type>(new type::String());

    if ( functionEmpty() ) {
        // We haven't added anything to the function, just discard.
        _globals_dtor_func->removeFromParent();
        _globals_dtor_func = nullptr;
    }

    popFunction();
}

llvm::Value* CodeGen::llvmGlobalIndex(Variable* var)
{
    auto i = _globals.find(var);
    assert(i != _globals.end());
    return i->second;
}

void CodeGen::createLinkerData()
{
    // Add them main meta information node.
    auto module_id = linkerModuleIdentifier();
    string name = ::util::fmt("%s.%s", string(symbols::MetaModule), module_id);

    auto old_md = _module->getNamedValue(name);

    if ( old_md )
        error("module meta data already exists");

    auto md  = _module->getOrInsertNamedMetadata(name);

    llvm::Value *version = llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvmContext()), 1);
    llvm::Value *id = llvm::MDString::get(llvmContext(), module_id);
    llvm::Value *file = llvm::MDString::get(llvmContext(), _hilti_module->path());
    llvm::Value *ctxtype = llvm::Constant::getNullValue(llvmTypePtr(llvmTypeExecutionContext()));

    // Note, the order here must match with the MetaModule* constants.
    md->addOperand(util::llvmMdFromValue(llvmContext(), version));
    md->addOperand(util::llvmMdFromValue(llvmContext(), id));
    md->addOperand(util::llvmMdFromValue(llvmContext(), file));
    md->addOperand(util::llvmMdFromValue(llvmContext(), ctxtype));

    // Add the line up of our globals.

    name = string(symbols::MetaGlobals) + "." + module_id;
    md  = _module->getOrInsertNamedMetadata(name);

    // Iterate through the collector globals here to guarantee same order as
    // in our global struct.
    for ( auto g : _collector->globals() ) {
        auto n = llvm::MDString::get(llvmContext(), scopedNameGlobal(g.get()));
        md->addOperand(util::llvmMdFromValue(llvmContext(), n));
    }

    // Add the MD function arrays that the linker will merge.

    if ( _module_init_func ) {
        md  = _module->getOrInsertNamedMetadata(symbols::MetaModuleInit);
        md->addOperand(util::llvmMdFromValue(llvmContext(), _module_init_func));
    }

    if ( _globals_init_func ) {
        md  = _module->getOrInsertNamedMetadata(symbols::MetaGlobalsInit);
        md->addOperand(util::llvmMdFromValue(llvmContext(), _globals_init_func));
    }

    if ( _globals_dtor_func ) {
        md  = _module->getOrInsertNamedMetadata(symbols::MetaGlobalsDtor);
        md->addOperand(util::llvmMdFromValue(llvmContext(), _globals_dtor_func));
    }
}

void CodeGen::createRtti()
{
    for ( auto i : _hilti_module->exportedTypes() )
        llvmRtti(i);
}

llvm::Value* CodeGen::cacheValue(const string& component, const string& key, llvm::Value* val)
{
    string idx = component + "::" + key;
    _value_cache.insert(std::make_pair(idx, val));
    return val;
}

llvm::Value* CodeGen::lookupCachedValue(const string& component, const string& key)
{
    string idx = component + "::" + key;
    auto i = _value_cache.find(idx);
    return (i != _value_cache.end()) ? i->second : nullptr;
}

llvm::Constant* CodeGen::cacheConstant(const string& component, const string& key, llvm::Constant* val)
{
    string idx = component + "::" + key;
    _constant_cache.insert(make_pair(idx, val));
    return val;
}

llvm::Constant* CodeGen::lookupCachedConstant(const string& component, const string& key)
{
    string idx = component + "::" + key;
    auto i = _constant_cache.find(idx);
    return (i != _constant_cache.end()) ? i->second : nullptr;
}

llvm::Type* CodeGen::cacheType(const string& component, const string& key, llvm::Type* type)
{
    string idx = component + "::" + key;
    _type_cache.insert(make_pair(idx, type));
    return type;
}

llvm::Type* CodeGen::lookupCachedType(const string& component, const string& key)
{
    string idx = component + "::" + key;
    auto i = _type_cache.find(idx);
    return (i != _type_cache.end()) ? i->second : nullptr;
}

codegen::TypeInfo* CodeGen::typeInfo(shared_ptr<hilti::Type> type)
{
    return _type_builder->typeInfo(type);
}

string CodeGen::uniqueName(const string& component, const string& str)
{
    string idx = component + "::" + str;

    auto i = _unique_names.find(idx);
    int cnt = (i != _unique_names.end()) ? i->second : 1;

    _unique_names.insert(make_pair(idx, cnt + 1));

    if ( cnt == 1 )
        return ::util::fmt(".hlt.%s.%s", str, linkerModuleIdentifier());
    else
        return ::util::fmt(".hlt.%s.%s.%d", str, linkerModuleIdentifier(), cnt);

}

llvm::Type* CodeGen::llvmType(shared_ptr<hilti::Type> type)
{
    return _type_builder->llvmType(type);
}

llvm::Constant* CodeGen::llvmInitVal(shared_ptr<hilti::Type> type)
{
    llvm::Constant* val = lookupCachedConstant("type.init_val", type->render());

    if ( val )
        return val;

    auto ti = typeInfo(type);
    assert(ti->init_val);

    return cacheConstant("type.init_val", type->render(), ti->init_val);
}

llvm::Constant* CodeGen::llvmRtti(shared_ptr<hilti::Type> type)
{
    string name = util::mangle(string("hlt_type_info_hlt_") + type->render(), true, nullptr, "", false);

    // We add the global here first and cache it before we call llvmRtti() so
    // the recursive calls function properly.
    name = ::util::strreplace(name, "_ref", "");
    name = ::util::strreplace(name, "_any", "");

    auto val = lookupCachedConstant("type.rtti", name);

    if ( val )
        return val;

    auto ti = llvmAddGlobal(name, llvmConstNull(_type_builder->llvmRttiType(type)), true);
    ti->setConstant(true);

    auto casted = llvm::ConstantExpr::getBitCast(ti, llvmTypePtr(llvmTypeRtti()));
    cacheConstant("type.rtti", name, casted);

    llvm::Constant* tival = _type_builder->llvmRtti(type);
    ti->setInitializer(tival);
    ti->setLinkage(llvm::GlobalValue::WeakODRLinkage);

    return casted;
}

#if 0
    llvm::Constant* cval = llvmConstNull(llvmTypePtr(llvmTypeRtti()));
    auto constant1 = llvmAddConst(name, cval, true);
    constant1->setLinkage(llvm::GlobalValue::WeakAnyLinkage);
    cacheConstant("type.rtti", type->render(), constant1);

    cval = _type_builder->llvmRtti(type);
    name = util::mangle(string("type_info_val_") + type->render(), true);
    auto constant2 = llvmAddConst(name, cval, true);
    constant2->setLinkage(llvm::GlobalValue::InternalLinkage);

    cval = llvm::ConstantExpr::getBitCast(constant2, llvmTypePtr(llvmTypeRtti()));
    constant1->setInitializer(llvm::ConstantExpr::getBitCast(constant2, llvmTypePtr(llvmTypeRtti())));

    return constant1;
}

llvm::Constant* CodeGen::llvmRtti(shared_ptr<hilti::Type> type)
{
    return llvmRttiPtr(type)->getInitializer();
}
#endif

llvm::Type* CodeGen::llvmTypeVoid() {
    return llvm::Type::getVoidTy(llvmContext());
}

llvm::Type* CodeGen::llvmTypeInt(int width) {
    return llvm::Type::getIntNTy(llvmContext(), width);
}

llvm::Type* CodeGen::llvmTypeFloat() {
    return llvm::Type::getFloatTy(llvmContext());
}

llvm::Type* CodeGen::llvmTypeDouble() {
    return llvm::Type::getDoubleTy(llvmContext());
}

llvm::Type* CodeGen::llvmTypeString() {
    return llvmTypePtr(llvmLibType("hlt.string"));
}

llvm::Type* CodeGen::llvmTypePtr(llvm::Type* t) {
    return llvm::PointerType::get(t ? t : llvmTypeInt(8), 0);
}

llvm::Type* CodeGen::llvmTypeExecutionContext() {
    return llvmLibType("hlt.execution_context");
}

llvm::Type* CodeGen::llvmTypeExceptionPtr() {
    return llvmTypePtr(llvmLibType("hlt.exception"));
}

llvm::Constant* CodeGen::llvmExceptionTypeObject(shared_ptr<type::Exception> excpt)
{
    auto libtype = excpt ? excpt->attributes().getAsString(attribute::LIBHILTI, "") : "hlt_exception_unspecified";

    if ( libtype.size() ) {
        // If it's a libhilti exception, create an extern declaration
        // pointing there.

        auto glob = lookupCachedConstant("type-exception-lib", libtype);

        if ( glob )
            return glob;

        auto g = llvmAddGlobal(libtype, llvmLibType("hlt.exception.type"), nullptr, true);
        g->setConstant(true);
        g->setInitializer(0);
        g->setLinkage(llvm::GlobalValue::ExternalLinkage);

        return cacheConstant("type-exception-lib", libtype, g);
    }

    else {
        // Otherwise, create the type (if we haven't already).

        assert(excpt->id());

        auto id = excpt->id()->pathAsString();
        auto glob = lookupCachedConstant("type-exception", id);

        if ( glob )
            return glob;

        auto name = llvmConstAsciizPtr(id);
        auto base = llvmExceptionTypeObject(ast::as<type::Exception>(excpt->baseType()));
        auto arg  = excpt->argType() ? llvmRtti(excpt->argType()) : llvmConstNull(llvmTypePtr(llvmTypeRtti()));

        base = llvm::ConstantExpr::getBitCast(base, llvmTypePtr());
        arg = llvm::ConstantExpr::getBitCast(arg, llvmTypePtr());

        constant_list elems;
        elems.push_back(name);
        elems.push_back(base);
        elems.push_back(arg);
        auto val = llvmConstStruct(llvmLibType("hlt.exception.type"), elems);
        glob = llvmAddConst(::util::fmt("exception-%s", excpt->id()->name().c_str()), val);

        return cacheConstant("type-exception", id, glob);
    }
}

llvm::Type* CodeGen::llvmTypeRtti()
{
    return llvmLibType("hlt.type_info");
}

llvm::Type* CodeGen::llvmTypeStruct(const string& name, const std::vector<llvm::Type*>& fields, bool packed)
{
    if ( name.size() )
        return llvm::StructType::create(llvmContext(), fields, name, packed);
    else
        return llvm::StructType::get(llvmContext(), fields, packed);
}

llvm::ConstantInt* CodeGen::llvmConstInt(int64_t val, int64_t width)
{
    assert(width <= 64);
    return llvm::ConstantInt::get(llvm::Type::getIntNTy(llvmContext(), width), val);
}

llvm::Constant* CodeGen::llvmConstDouble(double val)
{
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvmContext()), val);
}

llvm::Constant* CodeGen::llvmGEPIdx(int64_t idx)
{
    return llvmConstInt(idx, 32);
}

llvm::Constant* CodeGen::llvmConstNull(llvm::Type* t)
{
    if ( ! t )
        t = llvmTypePtr(llvmTypeInt(8)); // Will return "i8 *" aka "void *".

    return llvm::Constant::getNullValue(t);
}

llvm::Constant* CodeGen::llvmConstBytesEnd()
{
    auto t = llvmLibType("hlt.iterator.bytes");
    return llvmConstNull(t);
}

llvm::Constant* CodeGen::llvmConstArray(llvm::Type* t, const std::vector<llvm::Constant*>& elems)
{
    auto at = llvm::ArrayType::get(t, elems.size());
    return llvm::ConstantArray::get(at, elems);
}

llvm::Constant* CodeGen::llvmConstArray(const std::vector<llvm::Constant*>& elems)
{
    assert(elems.size() > 0);
    return llvmConstArray(elems.front()->getType(), elems);
}

llvm::Constant* CodeGen::llvmConstAsciiz(const string& str)
{
    auto c = lookupCachedConstant("const-asciiz", str);

    if ( c )
        return c;

    std::vector<llvm::Constant*> elems;

    for ( auto c : str )
        elems.push_back(llvmConstInt(c, 8));

    elems.push_back(llvmConstInt(0, 8));

    c = llvmConstArray(llvmTypeInt(8), elems);

    return cacheConstant("const-asciiz", str, c);
}

llvm::Constant* CodeGen::llvmConstAsciizPtr(const string& str)
{
    auto c = lookupCachedConstant("const-asciiz-ptr", str);

    if ( c )
        return c;

    auto cval = llvmConstAsciiz(str);
    auto ptr = llvmAddConst("asciiz", cval);

    c = llvm::ConstantExpr::getBitCast(ptr, llvmTypePtr());

    return cacheConstant("const-asciiz-ptr", str, c);

}

llvm::Constant* CodeGen::llvmConstStruct(const constant_list& elems, bool packed)
{
    if ( elems.size() )
        return llvm::ConstantStruct::getAnon(elems, packed);

    else {
        std::vector<llvm::Type*> empty;
        auto stype = llvmTypeStruct("", empty, packed);
        return llvmConstNull(stype);
    }
}

llvm::Constant* CodeGen::llvmConstStruct(llvm::Type* type, const constant_list& elems)
{
    return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(type), elems);
}

llvm::Value* CodeGen::llvmEnum(const string& label)
{
    auto expr = _hilti_module->body()->scope()->lookupUnique((std::make_shared<ID>(label)));

    if ( ! expr )
        throw ast::InternalError("llvmEnum: unknown enum label %s" + label);

    return llvmValue(expr);
}

llvm::Constant* CodeGen::llvmCastConst(llvm::Constant* c, llvm::Type* t)
{
    return llvm::ConstantExpr::getBitCast(c, t);
}

llvm::Value* CodeGen::llvmReinterpret(llvm::Value* val, llvm::Type* ntype)
{
    if ( val->getType() == ntype )
        return val;

    auto tmp = llvmCreateAlloca(val->getType());
    llvmCreateStore(val, tmp);
    auto casted = builder()->CreateBitCast(tmp, llvmTypePtr(ntype));
    return builder()->CreateLoad(casted);
}

llvm::Value* CodeGen::llvmStringFromData(const string& str)
{
    std::vector<llvm::Constant*> vec_data;
    for ( auto c : str )
        vec_data.push_back(llvmConstInt(c, 8));

    auto array = llvmConstArray(llvmTypeInt(8), vec_data);
    llvm::Constant* data = llvmAddConst("string", array);
    data = llvm::ConstantExpr::getBitCast(data, llvmTypePtr());

    value_list args;
    args.push_back(data);
    args.push_back(llvmConstInt(str.size(), 64));
    return llvmCallC(llvmLibFunction("hlt_string_from_data"), args, true, false);
}

llvm::Value* CodeGen::llvmStringPtr(const string& s)
{
    auto val = llvmString(s);
    auto ptr = llvmAddTmp("string", val->getType(), val);
    return ptr;
}

llvm::Value* CodeGen::llvmString(const string& s)
{
    if ( s.size() == 0 )
        // The empty string is represented by a null pointer.
        return llvmConstNull(llvmTypeString());

    return llvmStringFromData(s);
}

llvm::Value* CodeGen::llvmValueStruct(const std::vector<llvm::Value*>& elems, bool packed)
{
    // This is quite a cumbersome way to create a struct on the fly but it
    // seems it's the best we can do when inserting non-const values.

    // Determine the final type.
    std::vector<llvm::Type*> types;
    for ( auto e : elems )
        types.push_back(e->getType());

    auto stype = llvmTypeStruct("", types, packed);
    llvm::Value* sval = llvmConstNull(stype);

    for ( int i = 0; i < elems.size(); ++i )
        sval = llvmInsertValue(sval, elems[i], i);

    return sval;
}

llvm::Value* CodeGen::llvmValueStruct(llvm::Type* stype, const std::vector<llvm::Value*>& elems, bool packed)
{
    // This is quite a cumbersome way to create a struct on the fly but it
    // seems it's the best we can do when inserting non-const values.

    llvm::Value* sval = llvmConstNull(stype);

    for ( int i = 0; i < elems.size(); ++i )
        sval = llvmInsertValue(sval, elems[i], i);

    return sval;
}

llvm::GlobalVariable* CodeGen::llvmAddConst(const string& name, llvm::Constant* val, bool use_name_as_is)
{
    auto mname = use_name_as_is ? name : uniqueName("const", name);
    auto glob = new llvm::GlobalVariable(*_module, val->getType(), true, llvm::GlobalValue::PrivateLinkage, val, mname);
    return glob;
}

llvm::GlobalVariable* CodeGen::llvmAddGlobal(const string& name, llvm::Type* type, llvm::Constant* init, bool use_name_as_is)
{
    auto mname = use_name_as_is ? name : uniqueName("global", name);

    if ( ! init )
        init = llvmConstNull(type);

    auto glob = new llvm::GlobalVariable(*_module, type, false, llvm::GlobalValue::PrivateLinkage, init, mname);
    return glob;
}

llvm::GlobalVariable* CodeGen::llvmAddGlobal(const string& name, llvm::Constant* init, bool use_name_as_is)
{
    assert(init);
    return llvmAddGlobal(name, init->getType(), init, use_name_as_is);
}

llvm::Value* CodeGen::llvmAddLocal(const string& name, shared_ptr<Type> type, shared_ptr<Expression> init, bool hoisted)
{
    auto llvm_type = llvmType(type);

    bool init_in_entry_block = false;
    bool init_is_init_val = false;

    llvm::Value* llvm_init = nullptr;

    if ( ! init ) {
        init_in_entry_block = true;
        llvm_init = typeInfo(type)->init_val;
        init_is_init_val = true;
    }

    llvm::BasicBlock& block(function()->getEntryBlock());

    auto builder = newBuilder(&block, true);
    llvm::Value* local = 0;

    if ( ! hoisted ) {
        if ( init )
            llvm_init = llvmValue(init, type, false);

        local = builder->CreateAlloca(llvm_type, 0, name);
        builder->CreateStore(typeInfo(type)->init_val, local);

        if ( init_in_entry_block ) {
            pushBuilder(builder);
            llvmCreateStore(llvm_init, local);
            popBuilder();
        }

        else {
            if ( ! init_is_init_val )
                llvmCreateStore(llvm_init, local);
        }
    }

    else {
        // Hoist a reference type to the stack.
        assert(llvm_type->isPointerTy());
        llvm_type = llvm_type->getPointerElementType();
        local = builder->CreateAlloca(llvm_type, 0, name);

        if ( init )
            llvmValueInto(local, init, type);
    }

    _functions.back()->locals.insert(make_pair(name, std::make_tuple(local, type, hoisted)));

    delete builder;
    return local;
}

llvm::Value* CodeGen::llvmAddTmp(const string& name, llvm::Type* type, llvm::Value* init, bool reuse, int alignment)
{
    string tname = "__tmp_" + name;

    if ( reuse ) {
        if ( ! init )
            init = llvmConstNull(type);

        auto i = _functions.back()->tmps.find(tname);
        if ( i != _functions.back()->tmps.end() ) {
            auto tmp = std::get<0>(i->second);
            llvmCreateStore(init, tmp);
            return tmp;
        }
    }

    llvm::BasicBlock& block(function()->getEntryBlock());

    auto tmp_builder = newBuilder(&block, true);
    auto tmp = tmp_builder->CreateAlloca(type, 0, tname);

    if ( alignment )
        tmp->setAlignment(alignment);

    if ( init )
        // Must be done in original block.
        llvmCreateStore(init, tmp);
    else {
        // Do init entry block so that we don't overwrite this if the code
        // gets executed multiple times.
        pushBuilder(tmp_builder);
        llvmCreateStore(llvmConstNull(type), tmp);
        popBuilder();
    }

    _functions.back()->tmps.insert(std::make_pair(tname, std::make_tuple(tmp, nullptr, false)));

    return tmp;
}

llvm::Value* CodeGen::llvmAddTmp(const string& name, llvm::Value* init, bool reuse)
{
    assert(init);
    return llvmAddTmp(name, init->getType(), init, reuse);
}

llvm::FunctionType* CodeGen::llvmFunctionType(shared_ptr<type::Function> ftype)
{
    auto t = llvmAdaptFunctionArgs(ftype);
    return abi()->createFunctionType(t.first, t.second, ftype->callingConvention());
}

llvm::CallingConv::ID CodeGen::llvmCallingConvention(type::function::CallingConvention cc)
{
    // Determine the function's LLVM calling convention.
    switch ( cc ) {
     case type::function::HILTI:
     case type::function::HOOK:
     case type::function::CALLABLE:
        return llvm::CallingConv::Fast;

     case type::function::HILTI_C:
        return llvm::CallingConv::C;

     case type::function::C:
        return llvm::CallingConv::C;

     default:
        internalError("unknown calling convention in llvmCallingConvention");
    }

    // Can't be reached.
    assert(false);
    return llvm::CallingConv::Fast;
}

std::pair<llvm::Type*, std::vector<std::pair<string, llvm::Type*>>>
CodeGen::llvmAdaptFunctionArgs(shared_ptr<type::Function> ftype)
{
    parameter_list params;

    for ( auto p : ftype->parameters() )
        params.push_back(make_pair(p->id()->name(), p->type()));

    auto rtype = llvmType(ftype->result()->type());
    auto cc = ftype->callingConvention();

    return llvmAdaptFunctionArgs(rtype, params, cc, false);
}

std::pair<llvm::Type*, std::vector<std::pair<string, llvm::Type*>>>
CodeGen::llvmAdaptFunctionArgs(llvm::Type* rtype, parameter_list params, type::function::CallingConvention cc, bool skip_ctx)
{
    auto orig_rtype = rtype;

    std::vector<std::pair<string, llvm::Type*>> llvm_args;

    // Adapt the return type according to calling convention.
    switch ( cc ) {
     case type::function::HILTI:
     case type::function::CALLABLE:
        break;

     case type::function::HOOK:
        // Hooks always return a boolean.
        rtype = llvmTypeInt(1);
        break;

     case type::function::HILTI_C:
        // TODO: Do ABI stuff.
        break;

     case type::function::C:
        // TODO: Do ABI stuff.
        break;

     default:
        internalError("unknown calling convention in llvmAddFunction");
    }

    // Adapt parameters according to calling conventions.
    for ( auto p : params ) {

        switch ( cc ) {
         case type::function::HILTI:
         case type::function::HOOK:
         case type::function::CALLABLE: {
             auto arg_llvm_type = llvmType(p.second);
             llvm_args.push_back(make_pair(p.first, arg_llvm_type));
             break;
         }

         case type::function::HILTI_C: {
             auto ptype = p.second;

             if ( ast::isA<hilti::type::TypeType>(ptype) )
                 // Pass just RTTI for type arguments.
                 llvm_args.push_back(make_pair(string("ti_") + p.first, llvmTypePtr(llvmTypeRtti())));

             else {
                 TypeInfo* pti = typeInfo(ptype);
                 if ( pti->pass_type_info ) {
                     llvm_args.push_back(make_pair(string("ti_") + p.first, llvmTypePtr(llvmTypeRtti())));
                     llvm_args.push_back(make_pair(p.first, llvmTypePtr()));
                 }

                 else {
                     auto arg_llvm_type = llvmType(p.second);
                     llvm_args.push_back(make_pair(p.first, arg_llvm_type));
                 }
             }

             break;
         }
         case type::function::C: {
             auto arg_llvm_type = llvmType(p.second);
             llvm_args.push_back(make_pair(p.first, arg_llvm_type));
             break;
         }

     default:
            internalError("unknown calling convention in llvmAddFunction");
        }
    }

    // Add additional parameters our calling convention may need.
    switch ( cc ) {
     case type::function::HILTI:
     case type::function::CALLABLE:
        llvm_args.push_back(std::make_pair(symbols::ArgExecutionContext, llvmTypePtr(llvmTypeExecutionContext())));
        break;

     case type::function::HOOK:
        llvm_args.push_back(std::make_pair(symbols::ArgExecutionContext, llvmTypePtr(llvmTypeExecutionContext())));

        // Hooks with return value get an additional pointer to an instance
        // receiving it.
        if ( ! orig_rtype->isVoidTy() )
            llvm_args.push_back(std::make_pair("__rval", llvmTypePtr(orig_rtype)));
        break;

     case type::function::HILTI_C:
        llvm_args.push_back(std::make_pair(symbols::ArgException, llvmTypePtr(llvmTypeExceptionPtr())));

        if ( ! skip_ctx )
            llvm_args.push_back(std::make_pair(symbols::ArgExecutionContext, llvmTypePtr(llvmTypeExecutionContext())));
        break;

     case type::function::C:
        break;

     default:
        internalError("unknown calling convention in llvmAddFunction");
    }

    return std::make_pair(rtype, llvm_args);
}

llvm::Function* CodeGen::llvmAddFunction(const string& name, llvm::Type* rtype, parameter_list params, bool internal, type::function::CallingConvention cc, bool skip_ctx)
{
    auto llvm_linkage = internal ? llvm::Function::InternalLinkage : llvm::Function::ExternalLinkage;
    auto llvm_cc = llvmCallingConvention(cc);

    // See if we know that function already.
    auto func = _module->getFunction(name);

    if ( func ) {
        // Already created. But make sure cc and linkage are right.
        func->setCallingConv(llvm_cc);
        func->setLinkage(llvm_linkage);
        return func;
    }

    auto t = llvmAdaptFunctionArgs(rtype, params, cc, skip_ctx);

    func = abi()->createFunction(name, t.first, t.second, llvm_linkage, _module, cc);
    func->setCallingConv(llvm_cc);

    return func;
}

llvm::Function* CodeGen::llvmAddFunction(const string& name, llvm::Type* rtype, llvm_parameter_list params, bool internal, bool force_name)
{
    auto mangled_name = force_name ? name : mangleGlobal(std::make_shared<ID>(name));

    auto llvm_linkage = internal ? llvm::Function::InternalLinkage : llvm::Function::ExternalLinkage;
    auto llvm_cc = llvm::CallingConv::C;

    std::vector<llvm::Type*> func_args;
    for ( auto a : params )
        func_args.push_back(a.second);

    auto ftype = llvm::FunctionType::get(rtype, func_args, false);
    auto func = llvm::Function::Create(ftype, llvm_linkage, mangled_name, _module);

    func->setCallingConv(llvm_cc);

    auto i = params.begin();
    for ( auto a = func->arg_begin(); a != func->arg_end(); ++a, ++i )
        a->setName(i->first);

    return func;
}

llvm::Function* CodeGen::llvmAddFunction(shared_ptr<Function> func, bool internal, type::function::CallingConvention cc, const string& force_name, bool skip_ctx)
{
    string use_name = force_name;

    if ( cc == type::function::DEFAULT )
        cc = func->type()->callingConvention();

    if ( cc == type::function::C )
        use_name = func->id()->name();

    parameter_list params;

    for ( auto p : func->type()->parameters() )
        params.push_back(make_pair(p->id()->name(), p->type()));

    auto name = use_name.size() ? use_name : mangleGlobal(func->id(), func->module(), "", internal);

    auto rtype = llvmType(func->type()->result()->type());

    return llvmAddFunction(name, rtype, params, internal, cc, skip_ctx);
}

llvm::Function* CodeGen::llvmFunction(shared_ptr<Function> func, bool force_new)
{
    if ( func->type()->callingConvention() == type::function::C )
        // Don't mess with the name.
        return llvmAddFunction(func, false, type::function::C);

    bool is_hook = ast::isA<Hook>(func);
    bool has_impl = static_cast<bool>(func->body());

    bool internal = true;

    if ( func->module()->exported(func->id()) )
        internal = false;

    if ( func->type()->callingConvention() != type::function::HILTI )
        internal = false;

    if ( is_hook )
        internal = false;

    if ( ! has_impl )
        internal = false;

    string prefix;

    if ( is_hook )
        prefix = ::util::fmt(".hlt.%s", _hilti_module->id()->name());

    else if ( func->type()->callingConvention() == type::function::HILTI && ! internal )
        prefix = "hlt";

    int cnt = 0;

    string name;

    while ( true ) {
        name = mangleGlobal(func->id(), func->module(), prefix, internal);

        if ( ++cnt > 1 )
            name += ::util::fmt(".%d", cnt);

        auto llvm_func = _module->getFunction(name);

        if ( ! llvm_func )
            break;

        if ( ! force_new )
            return llvm_func;
    }

    return llvmAddFunction(func, internal, type::function::DEFAULT, name);
}

llvm::Function* CodeGen::llvmFunctionHookRun(shared_ptr<Hook> hook)
{
    string hname;
    auto hid = hook->id();

    if ( hid->isScoped() )
        hname = hid->pathAsString();
    else
        hname = ::util::fmt("%s::%s", hook->module()->id()->name(), hid->name());

    auto cname = util::mangle(hname, true);
    auto fval = lookupCachedValue("function-hook", cname);

    if ( fval )
        return llvm::cast<llvm::Function>(fval);

    auto func = llvmAddFunction(hook, false, type::function::HOOK, cname);

    // Add meta information for the hook.
    std::vector<llvm::Value *> vals;

    // MD node for the hook's name.
    auto name = hook->id()->pathAsString();

    if ( ! hook->id()->isScoped() )
        name = _hilti_module->id()->name() + "::" + name;

    // Record the name.
    vals.push_back(llvm::MDString::get(llvmContext(), name));

    // Record the the function we want to call for running the hook.
    vals.push_back(func);

    // Returnd the return type, if we have one.
    auto rtype = hook->type()->result()->type();
    if ( ! ast::isA<type::Void>(rtype) )
        vals.push_back(llvmConstNull(llvmType(rtype)));

    // Create the global hook declaration node and add our vals as subnode in
    // there. The linker will merge all the top-level entries.
    auto md  = _module->getOrInsertNamedMetadata(symbols::MetaHookDecls);
    md->addOperand(llvm::MDNode::get(llvmContext(), vals));

    cacheValue("function-hook", cname, func);
    return func;
}

void CodeGen::llvmAddHookMetaData(shared_ptr<Hook> hook, llvm::Value *llvm_func)
{
    std::vector<llvm::Value *> vals;

    // Record the hook's name.
    auto name = hook->id()->pathAsString();

    if ( ! hook->id()->isScoped() )
        name = _hilti_module->id()->name() + "::" + name;

    vals.push_back(llvm::MDString::get(llvmContext(), name));

    // Record the function we want to have called when running the hook.
    vals.push_back(llvm_func);

    // Record priority and group.
    auto ftype = ast::checkedCast<type::Hook>(hook->type());

    int64_t priority = hook->type()->attributes().getAsInt(attribute::PRIORITY, 0);
    int64_t group = hook->type()->attributes().getAsInt(attribute::GROUP, 0);

    vals.push_back(llvmConstInt(priority, 64));
    vals.push_back(llvmConstInt(group, 64));

    // Create/get the global hook implementation node and add our vals as
    // subnode in there. The linker will merge all the top-level entries.
    auto md  = llvmModule()->getOrInsertNamedMetadata(symbols::MetaHookImpls);
    md->addOperand(llvm::MDNode::get(llvmContext(), vals));
}

llvm::Function* CodeGen::llvmFunction(shared_ptr<ID> id)
{
    auto expr = _hilti_module->body()->scope()->lookupUnique(id);

    if ( ! expr )
        internalError(string("unknown function ") + id->pathAsString() + " in llvmFunction()");

    if ( ! ast::isA<expression::Function>(expr) )
        internalError(string("ID ") + id->name() + " is not a function in llvmFunction()");

    return llvmFunction(ast::as<expression::Function>(expr)->function());
}

llvm::Function* CodeGen::llvmFunction(const string& name)
{
    auto id = shared_ptr<ID>(new ID(name));
    return llvmFunction(id);
}

void CodeGen::llvmReturn(shared_ptr<Type> rtype, llvm::Value* result, bool result_cctored)
{
    if ( block()->getTerminator() ) {
        // Already terminated (and hopefully corrently).
        if ( result_cctored )
            assert(false);
        return;
    }

    auto state = _functions.back().get();

    if ( ! state->exit_block )
        state->exit_block = llvm::BasicBlock::Create(llvmContext(), ".exit", function());

    if ( result ) {
        if ( state->function->hasStructRetAttr() ) {
            auto func_rtype = state->function->arg_begin()->getType()->getPointerElementType();
            result = llvmReinterpret(result, func_rtype);
        }

        else {
            auto func_rtype = state->function->getReturnType();
            result = llvmReinterpret(result, func_rtype);
        }

        state->exits.push_back(std::make_pair(block(), result));

        if ( result_cctored )
            llvmDtor(result, rtype, false, "llvm-return2");
    }

    builder()->CreateBr(state->exit_block);
}

void CodeGen::llvmNormalizeBlocks()
{
    auto func = _functions.back()->function;
    llvm::Function::BasicBlockListType& blocks = func->getBasicBlockList();

    std::list<llvm::BasicBlock *> to_remove;

    for ( auto b = blocks.begin(); b != blocks.end(); b++ ) {
        if ( b->empty() && llvm::pred_begin(b) == llvm::pred_end(b) )
            to_remove.push_back(b);
    }

    for ( auto b : to_remove )
        b->eraseFromParent();
}


void CodeGen::llvmBuildExitBlock()
{
    auto state = _functions.back().get();

    if ( ! state->exit_block )
        return;

    ++_in_build_exit;

    auto exit_builder = newBuilder(state->exit_block);

    pushBuilder(exit_builder);

    llvm::PHINode* phi = nullptr;

    if ( state->exits.size() ) {
        auto rtype = state->exits.front().second->getType();
        phi = builder()->CreatePHI(rtype, state->exits.size());

        for ( auto e : state->exits )
            phi->addIncoming(e.second, e.first);
    }

    llvmBuildFunctionCleanup();

    auto leave_func = _functions.back()->leave_func;

    if ( leave_func ) {

        auto name = ::util::fmt("%s::%s", _hilti_module->id()->name().c_str(), leave_func->id()->name().c_str());

        if ( options().debug ) {
            string msg = string("leaving ") + name;
            llvmDebugPrint("hilti-flow", msg);
        }

        if ( options().profile >= 1 ) {
            // As this may be run in an exit block where we won't clean up
            // after us anymore, we do the string's mgt manually here.
            auto str = llvmStringFromData(string("func/") + name);
            llvmProfilerStop(str);
        }
    }

    if ( phi ) {

        if ( state->function->hasStructRetAttr() ) {
            // Need to store in argument.
            auto rt = state->function->arg_begin()->getType()->getPointerElementType();
            auto result = llvmReinterpret(phi, rt);
            builder()->CreateStore(result, state->function->arg_begin());
            builder()->CreateRetVoid();
        }

        else {
            auto result = llvmReinterpret(phi, state->function->getReturnType());
            builder()->CreateRet(result);
        }
    }

    else
        builder()->CreateRetVoid();

    --_in_build_exit;
}

void CodeGen::llvmDtorAfterInstruction(llvm::Value* val, shared_ptr<Type> type, bool is_ptr, bool is_hoisted, const string& location_addl)
{
    auto tmp = llvmAddTmp("dtor", val->getType());
    auto stmt = _stmt_builder->currentStatement();

    // Note: it's ok here if stmt is null, we use that for all tmps that
    // aren't directly associated with a statement.

    llvmGCAssign(tmp, val, type, true);
    _functions.back()->dtors_after_ins.insert(std::make_pair(stmt, std::make_tuple(tmp, is_ptr, type, false, is_hoisted, location_addl)));
}

void CodeGen::llvmMemorySafepoint(const std::string& where)
{
    CodeGen::value_list args;
    args.push_back(llvmExecutionContext());
    args.push_back(llvmConstAsciizPtr(where));

    llvmCallC("__hlt_memory_safepoint", args, false, false);
}

void CodeGen::llvmAdaptStackForSafepoint(bool pre)
{
    if ( ! pre )
        llvmCreateStackmap();

    for ( auto l : liveValues() ) {
        auto val = std::get<0>(l);
        auto type = std::get<1>(l);
        auto is_ptr = std::get<2>(l);

        if ( pre )
            llvmCctor(val, type, is_ptr, "adapt-for-savepoint-pre");
        else
            llvmDtor(val, type, is_ptr, "adapt-for-savepoint-post");
    }
}

CodeGen::live_list CodeGen::liveValues()
{
    live_list lives;

    // If null, we're inside some internal function.
    if ( _functions.back()->leave_func ) {

        auto ln = _stmt_builder->liveness();
        auto in = *ln.in;
        auto out = *ln.out;
        auto dead = *ln.dead;

        for ( auto l : in ) {

            if ( dead.find(l) != dead.end() )
                continue;

            auto val = llvmValueAddress(l->expression);
            auto type = l->expression->type();

            if ( l->expression->hoisted() )
                continue;

            assert(val);
            lives.push_back(std::make_tuple(val, type, true));
        }
    }

    return lives;
}

void CodeGen::llvmCreateStackmap()
{
#ifndef HAVE_LLVM_35
    return;
#endif

    string fid = ::util::fmt("%s:%d", _functions.back()->function->getName().str(), ++_functions.back()->stackmap_id);
    uint64_t pid = ::util::hash(fid);

#ifdef HAVE_LLVM_34
    CodeGen::value_list args = { llvmConstInt(pid, 32), llvmConstInt(0, 32) };
    std::vector<llvm::Type *> tys = { llvmTypeInt(32), llvmTypeInt(32) };
#else
    CodeGen::value_list args = { llvmConstInt(pid, 64), llvmConstInt(0, 32) };
    std::vector<llvm::Type *> tys = { llvmTypeInt(64), llvmTypeInt(32) };
#endif

    for ( auto l : liveValues() ) {
        auto val = std::get<0>(l);
        auto type = std::get<1>(l);
        auto is_ptr = std::get<2>(l);

        if ( is_ptr )
            val = builder()->CreateLoad(val);

        // LLVM 3.5 crashes if we pass in an i1 here. However, we don't need
        // to do that anyways, so just limit to types that are interested to
        // the memory management.
        if ( val->getType()->isPointerTy() || val->getType()->isAggregateType() )
            args.push_back(val);
    }

    // The normal intrinsic workflow doesn't work here for some reason. Probably the varargs.
    auto stackmap = _module->getOrInsertFunction(llvm::Intrinsic::getName(llvm::Intrinsic::experimental_stackmap),
                                                 llvm::FunctionType::get(llvmTypeVoid(), tys, 1));
    assert(stackmap);

    llvmCreateCall(stackmap, args);
}

void CodeGen::llvmClearLocalOnException(shared_ptr<Expression> expr)
{
    _functions.back()->locals_cleared_on_excpt.push_back(expr);
}

void CodeGen::llvmFlushLocalsClearedOnException()
{
    _functions.back()->locals_cleared_on_excpt.clear();
}

void CodeGen::llvmClearLocalAfterInstruction(shared_ptr<Expression> expr, const string& location_addl)
{
    auto stmt = _stmt_builder->currentStatement();

    // Note: it's ok here if stmt is null, we use that for all tmps that
    // aren't directly associated with a statement.

    _functions.back()->dtors_after_ins_exprs.insert(std::make_pair(stmt, std::make_pair(expr, location_addl)));
}

void CodeGen::llvmBuildInstructionCleanup(bool flush, bool dont_do_locals)
{
    // TODO: This function is getting messy ... At least the "local" stuff
    // should be factored out.

    auto stmt = _stmt_builder->currentStatement();

    // Note: it's ok here if stmt is null, we use that for all tmps that
    // aren't directly associated with a statement.

    // Note: This method may run mutiple times per instruction, and must hece
    // be safe against doing so. That's normally the case because it removes
    // all tmps once cleaned up.

    if ( _functions.back()->dtors_after_ins_exprs.size() || _functions.back()->dtors_after_ins.size() )
        llvmDebugPrint("hilti-trace", string("begin-instr-cleanup in ") + _functions.back()->function->getName().str());

    auto range = _functions.back()->dtors_after_ins_exprs.equal_range(stmt);
    for ( auto i = range.first; i != range.second; i++ ) {
        if ( dont_do_locals )
            continue;

        auto tupl = (*i).second;
        auto expr = std::get<0>(tupl);
        auto loc_addl = std::get<1>(tupl);

        auto type = expr->type();

        if ( expr->hoisted() ) {
            auto tmp = llvmValue(expr);
            llvmDestroy(tmp, type, "instr-cleanup-1-hoist/" + loc_addl);
            continue;
        }

        auto tmp = llvmValueAddress(expr);

        for ( auto l : _functions.back()->locals ) {
            if ( std::get<0>(l.second) == tmp ) {
                llvmGCClear(tmp, type, "instr-cleanup-1/" + loc_addl);
                break;
            }
        }
    }

    auto range2 = _functions.back()->dtors_after_ins.equal_range(stmt);
    for ( auto i = range2.first; i != range2.second; i++ ) {
        auto unref = (*i).second;
        auto tmp = std::get<0>(unref);
        auto ptr = std::get<1>(unref);
        auto type = std::get<2>(unref);
        auto local = std::get<3>(unref);
        auto hoisted = std::get<4>(unref);
        auto loc_addl = std::get<5>(unref);

        if ( local ) {
            if ( dont_do_locals )
                continue;

            if ( hoisted ) {
                llvmDestroy(tmp, type, "instr-cleanup-2-hoist/" + loc_addl);
                continue;
            }

            // See if we indeed know that address as a local. If not, it's a
            // const parameter that we don't need to unref.
            for ( auto l : _functions.back()->locals ) {
                if ( std::get<0>(l.second) == tmp ) {
                    llvmGCClear(std::get<0>(l.second), std::get<1>(l.second), string("instr-cleanup-2/") + loc_addl);
                }
            }

            continue;
        }

        auto dtor = newBuilder("dtor-tmp");
        auto cont = newBuilder("cont");
        auto val = builder()->CreateLoad(tmp);

        llvm::Value* ptr_val = ptr ? val : tmp;

        if ( val->getType()->isStructTy() ) {
            llvmDtor(ptr_val, type, true, "function-instruction-struct-tmp/" + loc_addl);
            llvmCreateStore(llvmConstNull(val->getType()), tmp);
            continue;
        }

        auto is_null = llvmCreateIsNull(val);
        llvmCreateCondBr(is_null, cont, dtor);

        pushBuilder(dtor);

        llvmDtor(ptr_val, type, true, "instruction-cleanup-tmp/" + loc_addl);

        llvmCreateStore(llvmConstNull(val->getType()), tmp);
        llvmCreateBr(cont);
        popBuilder();

        pushBuilder(cont);

        // Leave on stack.
    }

    if ( _functions.back()->dtors_after_ins_exprs.size() || _functions.back()->dtors_after_ins.size() )
        llvmDebugPrint("hilti-trace", "end-instr-cleanup");

    if ( flush ) {
        _functions.back()->dtors_after_ins_exprs.erase(stmt);
        _functions.back()->dtors_after_ins.erase(stmt);
    }
}

void CodeGen::llvmDiscardInstructionCleanup()
{
    auto stmt = _stmt_builder->currentStatement();

    // Note: it's ok here if stmt is null, we use that for all tmps that
    // aren't directly associated with a statement.

    _functions.back()->dtors_after_ins.erase(stmt);
}

void CodeGen::setInstructionCleanupAfterCall()
{
    _functions.back()->dtors_after_call = true;
}

void CodeGen::llvmBuildFunctionCleanup()
{
    // Need to destroy locals hoisted to the stack.
    for ( auto l : _functions.back()->locals ) {
        auto val = std::get<0>(l.second);
        auto type = std::get<1>(l.second);
        auto hoisted = std::get<2>(l.second);

        if ( hoisted )
            llvmDestroy(val, type, "function-cleanup-hoist");
    }
}

llvm::Value* CodeGen::llvmGEP(llvm::Value* addr, llvm::Value* i1, llvm::Value* i2, llvm::Value* i3, llvm::Value* i4)
{
    std::vector<llvm::Value *> idx;

    if ( i1 )
        idx.push_back(i1);

    if ( i2 )
        idx.push_back(i2);

    if ( i3 )
        idx.push_back(i3);

    if ( i4 )
        idx.push_back(i4);

    return builder()->CreateGEP(addr, idx);
}

llvm::Constant* CodeGen::llvmGEP(llvm::Constant* addr, llvm::Value* i1, llvm::Value* i2, llvm::Value* i3, llvm::Value* i4)
{
    std::vector<llvm::Value *> idx;

    if ( i1 )
        idx.push_back(i1);

    if ( i2 )
        idx.push_back(i2);

    if ( i3 )
        idx.push_back(i3);

    if ( i4 )
        idx.push_back(i4);

    return llvm::ConstantExpr::getGetElementPtr(addr, idx);
}

llvm::CallInst* CodeGen::llvmCallC(llvm::Value* llvm_func, const value_list& args, bool add_hiltic_args, bool excpt_check)
{
    value_list call_args = args;

    llvm::Value* excpt = nullptr;

    if ( add_hiltic_args ) {
        excpt = llvmAddTmp("excpt", llvmTypeExceptionPtr(), 0, true);
        call_args.push_back(excpt);
        call_args.push_back(llvmExecutionContext());
    }

    auto result = llvmCreateCall(llvm_func, call_args);

    if ( excpt_check && excpt )
        llvmCheckCException(excpt);

    return result;
}

llvm::CallInst* CodeGen::llvmCallC(const string& llvm_func, const value_list& args, bool add_hiltic_args, bool excpt_check)
{
    auto f = llvmLibFunction(llvm_func);
    return llvmCallC(f, args, add_hiltic_args, excpt_check);
}

llvm::CallInst* CodeGen::llvmCallIntrinsic(llvm::Intrinsic::ID intr, std::vector<llvm::Type*> tys, const value_list& args)
{
    auto func = llvm::Intrinsic::getDeclaration(_module, intr, tys);
    assert(func);
    return llvmCreateCall(func, args);
}

void CodeGen::llvmRaiseException(const string& exception, shared_ptr<Node> node, llvm::Value* arg)
{
    return llvmRaiseException(exception, node->location(), arg);
}

llvm::Value* CodeGen::llvmExceptionNew(const string& exception, const Location& l, llvm::Value* arg)
{
    auto expr = _hilti_module->body()->scope()->lookupUnique((std::make_shared<ID>(exception)));

    if ( ! expr )
        internalError(::util::fmt("unknown exception %s", exception.c_str()), l);

    auto type = ast::as<expression::Type>(expr)->typeValue();
    assert(type);

    auto etype = ast::as<type::Exception>(type);
    assert(etype);

    if ( arg )
        arg = builder()->CreateBitCast(arg, llvmTypePtr());

    CodeGen::value_list args;
    args.push_back(llvmExceptionTypeObject(etype));
    args.push_back(arg ? arg : llvmConstNull());
    args.push_back(llvmLocationString(l));
    args.push_back(llvmExecutionContext());

    return llvmCallC("hlt_exception_new", args, false, false);
}

llvm::Value* CodeGen::llvmExceptionArgument(llvm::Value* excpt)
{
    value_list args = { excpt };
    return llvmCallC("hlt_exception_arg", args, false, false);
}

llvm::Value* CodeGen::llvmExceptionFiber(llvm::Value* excpt)
{
    value_list args = { excpt };
    return llvmCallC("__hlt_exception_fiber", args, false, false);
}

void CodeGen::llvmRaiseException(const string& exception, const Location& l, llvm::Value* arg)
{
    auto excpt = llvmExceptionNew(exception, l, arg);
    llvmRaiseException(excpt, false);
}

void CodeGen::llvmRaiseException(llvm::Value* excpt, bool dtor)
{
    value_list args;
    args.push_back(llvmExecutionContext());
    args.push_back(excpt);
    llvmCallC("__hlt_context_set_exception", args, false, false);

    if ( dtor ) {
        auto ty = builder::reference::type(builder::exception::type(nullptr, nullptr));
        llvmDtor(excpt, ty, false, "raise-exception");
    }

    llvmTriggerExceptionHandling(true);
}

void CodeGen::llvmRethrowException()
{
    auto func = _functions.back()->function;

    // If the function has HILTI-C linkage, transfer the exception over into
    // the corresponding parameter.
    if ( _functions.back()->cc == type::function::HILTI_C ) {
        auto ctx_excpt = llvmCurrentException();
        auto last_arg = func->arg_end();
        llvmGCAssign(--(--last_arg), ctx_excpt, builder::reference::type(builder::exception::typeAny()), false);
        llvmClearException();
    }

    auto rt = func->getReturnType();

    if ( rt->isVoidTy() && _functions.back()->function->hasStructRetAttr() )
        rt = func->arg_begin()->getType()->getPointerElementType();

    if ( rt->isVoidTy() )
        llvmReturn();
    else
        // This simply returns with a null value. The caller will check for a thrown exception.
        llvmReturn(0, llvmConstNull(rt));
}

void CodeGen::llvmClearException()
{
    value_list args;
    args.push_back(llvmExecutionContext());
    llvmCallC("__hlt_context_clear_exception", args, false, false);
}

llvm::Value* CodeGen::llvmCurrentException()
{
    value_list args;
    args.push_back(llvmExecutionContext());
    return llvmCallC("__hlt_context_get_exception", args, false, false);
}

llvm::Value* CodeGen::llvmCurrentFiber()
{
    value_list args;
    args.push_back(llvmExecutionContext());
    return llvmCallC("__hlt_context_get_fiber", args, false, false);
}

llvm::Value* CodeGen::llvmCurrentVID()
{
    value_list args;
    args.push_back(llvmExecutionContext());
    return llvmCallC("__hlt_context_get_vid", args, false, false);
}

llvm::Value* CodeGen::llvmCurrentThreadContext()
{
    if ( ! _hilti_module->executionContext() )
        return nullptr;

    value_list args;
    args.push_back(llvmExecutionContext());
    auto ctx = llvmCallC("__hlt_context_get_thread_context", args, false, false);
    return builder()->CreateBitCast(ctx, llvmType(_hilti_module->executionContext()));
}

void CodeGen::llvmSetCurrentThreadContext(shared_ptr<Type> type, llvm::Value* ctx)
{
    value_list args;
    args.push_back(llvmExecutionContext());
    args.push_back(llvmRtti(type));
    args.push_back(builder()->CreateBitCast(ctx, llvmTypePtr()));
    llvmCallC("__hlt_context_set_thread_context", args, false, false);
}

#if 0
llvm::Value* CodeGen::llvmSetCurrentFiber(llvm::Value* fiber)
{
    value_list args;
    args.push_back(llvmExecutionContext());
    args.push_back(fiber);
    return llvmCallC("__hlt_context_set_fiber", args, false, false);
}

llvm::Value* CodeGen::llvmClearCurrentFiber()
{
    value_list args;
    args.push_back(llvmExecutionContext());
    args.push_back(llvmConstNull(llvmTypePtr(llvmLibType("hlt.fiber"))));
    return llvmCallC("__hlt_context_set_fiber", args, false, false);
}

#endif


void CodeGen::llvmCheckCException(llvm::Value* excpt, bool reraise)
{
    if ( _in_build_exit )
        // Can't handle exeptions in exit block.
        return;

    auto eval = builder()->CreateLoad(excpt);
    auto is_null = llvmExpect(llvmCreateIsNull(eval), llvmConstInt(1, 1));
    auto cont = newBuilder("no-excpt");
    auto raise = newBuilder("excpt-c");

    llvmCreateCondBr(is_null, cont, raise);

    pushBuilder(raise);

    if ( reraise )
        llvmRaiseException(eval, true);

    else {
        value_list args;
        args.push_back(llvmExecutionContext());
        args.push_back(builder()->CreateLoad(excpt));
        llvmCallC("__hlt_context_set_exception", args, false, false);

        auto ty = builder::reference::type(builder::exception::typeAny());
        llvmDtor(excpt, ty, true, "llvmDoCall/excpt");
        llvmCreateBr(cont);
    }

    popBuilder();

    pushBuilder(cont); // leave on stack.
}

void CodeGen::llvmCheckException()
{
    if ( _in_build_exit )
        // Can't handle exeptions in exit block.
        return;

    if ( ! _functions.back()->abort_on_excpt ) {
        llvmTriggerExceptionHandling(false);
        return;
    }

    // In the current function, an exception triggers an abort.

    llvmBuildInstructionCleanup(false);

    auto excpt = llvmCurrentException();
    auto is_null = llvmExpect(llvmCreateIsNull(excpt), llvmConstInt(1, 1));
    auto cont = newBuilder("no-excpt");
    auto abort = newBuilder("excpt-abort");

    llvmCreateCondBr(is_null, cont, abort);

    pushBuilder(abort);
    llvmCallC("__hlt_exception_print_uncaught_abort", { excpt, llvmExecutionContext() }, false, false);
    builder()->CreateUnreachable();
    popBuilder();

    pushBuilder(cont); // leave on stack.
}

llvm::Value* CodeGen::llvmMatchException(const string& name, llvm::Value* excpt)
{
    auto expr = _hilti_module->body()->scope()->lookupUnique((std::make_shared<ID>(name)));

    if ( ! expr )
        internalError(::util::fmt("unknown exception %s", name.c_str()));

    auto type = ast::as<expression::Type>(expr)->typeValue();
    assert(type);

    auto etype = ast::as<type::Exception>(type);
    assert(etype);

    return llvmMatchException(etype, excpt);
}

llvm::Value* CodeGen::llvmMatchException(shared_ptr<type::Exception> etype, llvm::Value* excpt)
{
    CodeGen::value_list args = { excpt, llvmExceptionTypeObject(etype) };
    auto match = llvmCallC("__hlt_exception_match", args, false, false);
    return builder()->CreateICmpNE(match, llvmConstInt(0, 8));
}

void CodeGen::llvmTriggerExceptionHandling(bool known_exception)
{
#if 0
    if ( _in_check_exception )
        // Make sure we don't recurse.
        return;

    ++_in_check_exception;
#endif

    // If we don't know yet whether we have an exception, check that.
    auto cont = newBuilder("excpt-check-done");
    IRBuilder* catch_ = nullptr;
    llvm::Value* current = 0;

    if ( ! known_exception ) {
        catch_ = newBuilder("excpt-catch");
        current = llvmCurrentException();
        auto is_null = llvmExpect(llvmCreateIsNull(current), llvmConstInt(1, 1));
        llvmCreateCondBr(is_null, cont, catch_);
        pushBuilder(catch_);
    }

    llvmDebugPrint("hilti-flow", "exception raised");

    llvmBuildInstructionCleanup(false);

    // Sort catches from most specific to least specific.
    auto catches = _functions.back()->catches;

    typedef std::pair<shared_ptr<Expression>, shared_ptr<type::Exception>> pa;
    catches.sort([](const pa& a, const pa& b) {
        return a.second->level() >= b.second->level(); // reverse sort
    });

    for ( auto c : catches ) {
        auto next = newBuilder("excpt-catch-next");

        if ( ! current )
            current = llvmCurrentException();

        auto match = llvmMatchException(c.second, current);
        auto catch_bb = llvm::cast<llvm::BasicBlock>(llvmValue(c.first));

        builder()->CreateCondBr(match, catch_bb, next->GetInsertBlock());

        pushBuilder(next);
    }

    for ( auto l : _functions.back()->locals_cleared_on_excpt ) {

        if ( l->hoisted() ) {
            auto tmp = llvmValue(l);
            llvmDestroy(tmp, l->type(), "trigger-excpt-handling/hoist");
            continue;
        }

        auto tmp = llvmValueAddress(l);

        if ( tmp )
            llvmGCClear(tmp, l->type(), "trigger-excpt-handling");

        else {
            // A constant parameter.
            auto p = ast::checkedCast<expression::Parameter>(l);
            assert(p->parameter()->constant());
            llvmDtor(llvmValue(l), l->type(), false, "trigger-excpt-handling/const-param");
        }
    }

    llvmRethrowException();

    // if ( catch_ )
    //    popBuilder(catch_);

    pushBuilder(cont); // Leave on stack.

#if 0
    --_in_check_exception;
#endif
}


llvm::CallInst* CodeGen::llvmCreateCall(llvm::Value *callee, llvm::ArrayRef<llvm::Value *> args, const llvm::Twine &name)
{
    return util::checkedCreateCall(builder(), "CodeGen", callee, args, name);
}

llvm::CallInst* CodeGen::llvmCreateCall(llvm::Value *callee, const llvm::Twine &name)
{
    std::vector<llvm::Value*> no_params;
    return util::checkedCreateCall(builder(), "CodeGen", callee, no_params, name);
}

static void _dumpStore(llvm::Value *val, llvm::Value *ptr, const string& where, const string& msg)
{
    llvm::raw_os_ostream os(std::cerr);

    os << "\n";
    os << "=== LLVM store mismatch in " << where << ": " << msg << "\n";
    os << "\n";
    os << "-- Value type:\n";
    val->getType()->print(os);
    os << "\n";
    os << "-- Target type:\n";
    ptr->getType()->print(os);
    os << "\n";
    os.flush();

    ::util::abort_with_backtrace();
}

llvm::StoreInst* CodeGen::llvmCreateStore(llvm::Value *val, llvm::Value *ptr, bool isVolatile)
{
    auto ptype = ptr->getType();
    auto p = llvm::isa<llvm::PointerType>(ptype);

    if ( ! p )
        _dumpStore(val, ptr, "CodeGen", "target is not of pointer type");

    if ( llvm::cast<llvm::PointerType>(ptype)->getElementType() != val->getType() )
        _dumpStore(val, ptr, "CodeGen", "operand types do not match");

    return builder()->CreateStore(val, ptr, isVolatile);
}

llvm::AllocaInst *CodeGen::llvmCreateAlloca(llvm::Type* t, llvm::Value *array_size, const llvm::Twine& name)
{
    if ( ! function() )
        // Not sure this can actually happen.
        return builder()->CreateAlloca(t);

    llvm::BasicBlock& block(function()->getEntryBlock());

    return newBuilder(&block, true)->CreateAlloca(t, array_size, name);
}

llvm::BranchInst* CodeGen::llvmCreateBr(IRBuilder* b)
{
    return builder()->CreateBr(b->GetInsertBlock());
}

llvm::BranchInst* CodeGen::llvmCreateCondBr(llvm::Value* cond, IRBuilder* true_, IRBuilder* false_)
{
    return builder()->CreateCondBr(cond, true_->GetInsertBlock(), false_->GetInsertBlock());
}

llvm::Value* CodeGen::llvmCreateIsNull(llvm::Value *arg, const llvm::Twine &name)
{
    if ( arg->getType()->isFloatingPointTy() )
        return builder()->CreateFCmpOEQ(arg, llvm::Constant::getNullValue(arg->getType()), name);

    else
        return builder()->CreateIsNull(arg);
}

llvm::Value* CodeGen::llvmInsertValue(llvm::Value* aggr, llvm::Value* val, unsigned int idx)
{
    if ( llvm::isa<llvm::VectorType>(aggr->getType()) )
        return builder()->CreateInsertElement(aggr, val, llvmConstInt(idx, 32));

    std::vector<unsigned int> vec;
    vec.push_back(idx);
    return builder()->CreateInsertValue(aggr, val, vec);
}

llvm::Constant* CodeGen::llvmConstInsertValue(llvm::Constant* aggr, llvm::Constant* val, unsigned int idx)
{
    if ( llvm::isa<llvm::VectorType>(aggr->getType()) )
        return llvm::ConstantExpr::getInsertElement(aggr, val, llvmConstInt(idx, 32));

    std::vector<unsigned int> vec;
    vec.push_back(idx);
    return llvm::ConstantExpr::getInsertValue(aggr, val, vec);
}

llvm::Value* CodeGen::llvmExtractValue(llvm::Value* aggr, unsigned int idx)
{
    if ( llvm::isa<llvm::VectorType>(aggr->getType()) )
        return builder()->CreateExtractElement(aggr, llvmConstInt(idx, 32));

    std::vector<unsigned int> vec;
    vec.push_back(idx);
    return builder()->CreateExtractValue(aggr, vec);
}

llvm::Constant* CodeGen::llvmConstExtractValue(llvm::Constant* aggr, unsigned int idx)
{
    if ( llvm::isa<llvm::VectorType>(aggr->getType()) )
        return llvm::ConstantExpr::getExtractElement(aggr, llvmConstInt(idx, 32));

    std::vector<unsigned int> vec;
    vec.push_back(idx);
    return llvm::ConstantExpr::getExtractValue(aggr, vec);
}

std::pair<llvm::Value*, llvm::Value*> CodeGen::llvmBuildCWrapper(shared_ptr<Function> func)
{
    // Name must match with ProtoGen::visit(declaration::Function* f).
    auto name = mangleGlobal(func->id(), func->module());

    auto rf1 = lookupCachedValue("c-wrappers", "entry-" + name);
    auto rf2 = lookupCachedValue("c-wrappers", "resume-" + name);

    if ( rf1 )
        return std::make_pair(rf1, rf2);

    auto ftype = func->type();
    auto rtype = ftype->result()->type();

    if ( ! func->body() ) {
        // No implementation, nothing to do here.
        internalError("llvmBuildCWrapper: not implemented for function without body; should return prototypes.");
        return std::make_pair((llvm::Value*)nullptr, (llvm::Value*)nullptr);
    }

    assert(ftype->callingConvention() == type::function::HILTI);
    assert(func->body());

    // Build the entry function.

    auto llvm_func = llvmAddFunction(func, false, type::function::HILTI_C, name, false);
    rf1 = llvm_func;

    pushFunction(llvm_func);
    //_functions.back()->context = llvmGlobalExecutionContext();  // FIXME: Use context argument.

    llvmClearException();

    expr_list params;

    auto arg = llvm_func->arg_begin();
    for ( auto a : ftype->parameters() )
        params.push_back(builder::codegen::create(a->type(), arg++));

    llvm::Value* result = 0;

    if ( ! func->type()->mayYield() ) {
        llvmDebugPrint("hilti-flow", ::util::fmt("entering entry wrapper for %s", func->id()->pathAsString()));
        result = llvmCall(func, params, true, false); // +1 as otherwise the subsequent safepoint would delete it.
        // When we use a fiber, that takes care of inserting a safepoint.
        // Without fiber, we need to do it ourselves.
        llvmMemorySafepoint("cstub-noyield");
        llvmDebugPrint("hilti-flow", ::util::fmt("left entry wrapper for %s", func->id()->pathAsString()));
    }

    else {
        llvmDebugPrint("hilti-flow", ::util::fmt("entering entry fiber for %s", func->id()->pathAsString()));
        // +1 as otherwise the subsequent safepoint would delete it.
        result = llvmCallInNewFiber(func, params, true);
        llvmDebugPrint("hilti-flow", ::util::fmt("left entry fiber for %s", func->id()->pathAsString()));
    }

    // Unref the result's +1.
    if ( ! rtype->equal(shared_ptr<Type>(new type::Void())) )
        llvmDtor(result, rtype, false, "cwrapper/result-adjust");

    // Copy exception over.
    auto ctx_excpt = llvmCurrentException();
    auto last_arg = llvm_func->arg_end();
    llvmGCAssign(--(--last_arg), ctx_excpt, builder::reference::type(builder::exception::typeAny()), false);

    if ( rtype->equal(shared_ptr<Type>(new type::Void())) )
        llvmReturn();
    else
        llvmReturn(rtype, result, false);

    popFunction();

    if ( ! func->type()->mayYield() )
        return std::make_pair(rf1, nullptr);

    // Build the resume function.

        // Name must match with ProtoGen::visit(declaration::Function* f).
    name = util::mangle(func->id()->name() + "_resume", true, func->module()->id(), "", false);

    parameter_list fparams = { std::make_pair("yield_excpt", builder::reference::type(builder::exception::typeAny())) };
    llvm_func = llvmAddFunction(name, llvmType(rtype), fparams, false, type::function::HILTI_C, false);
    rf2 = llvm_func;

    pushFunction(llvm_func);
    //_functions.back()->context = llvmGlobalExecutionContext(); // FIXME: Use context argument.

    llvmClearException();

    auto yield_excpt = llvm_func->arg_begin();

    if ( llvm_func->hasStructRetAttr() )
        ++yield_excpt;

    auto fiber = llvmExceptionFiber(yield_excpt);
    fiber = builder()->CreateBitCast(fiber, llvmTypePtr(llvmLibType("hlt.fiber")));

    value_list eargs = { yield_excpt };
    llvmCallC("__hlt_exception_clear_fiber", eargs, false, false);

    llvmDtor(yield_excpt, builder::reference::type(builder::exception::typeAny()), false, "c-wrapper/resume");

    llvmDebugPrint("hilti-flow", ::util::fmt("entering resume fiber for %s", func->id()->pathAsString()));
    result = llvmFiberStart(fiber, rtype);
    llvmDebugPrint("hilti-flow", ::util::fmt("left resume fiber for %s", func->id()->pathAsString()));

    // Result is +1 here, as that's how the entry fiber calls it. Unref.
    if ( ! rtype->equal(shared_ptr<Type>(new type::Void())) )
        llvmDtor(result, rtype, false, "cwrapper/result-adjust");

    // Copy exception over.
    ctx_excpt = llvmCurrentException();
    llvmGCAssign(++yield_excpt, ctx_excpt, builder::reference::type(builder::exception::typeAny()), false, false);

    if ( rtype->equal(shared_ptr<Type>(new type::Void())) )
        llvmReturn();
    else
        llvmReturn(rtype, result, false);

    popFunction();

    cacheValue("c-wrappers", "entry-" + name, rf1);
    cacheValue("c-wrappers", "resume-" + name, rf2);

    return std::make_pair(rf1, rf2);
}

llvm::Value* CodeGen::llvmCall(llvm::Value* llvm_func, shared_ptr<type::Function> ftype, const expr_list args, bool result_cctored, bool excpt_check, call_exception_callback_t excpt_callback)
{
    auto result = llvmDoCall(llvm_func, nullptr, nullptr, ftype, args, result_cctored, nullptr, excpt_check, excpt_callback);
    return result;
}

llvm::Value* CodeGen::llvmCallInNewFiber(shared_ptr<Function> func, const expr_list args, bool result_cctored)
{
    auto ftype = func->type();
    auto rtype = ftype->result()->type();

    // Create a struct value with all the arguments, plus the current context.

    CodeGen::type_list stypes;

    for ( auto a : args )
        stypes.push_back(llvmType(a->type()));

    auto sty = llvm::cast<llvm::StructType>(llvmTypeStruct("", stypes));
    llvm::Value* sval = llvmConstNull(sty);

    for ( int i = 0; i < args.size(); ++i ) {
        auto val = llvmValue(args[i]);
        val = llvmReinterpret(val, stypes[i]);
        sval = llvmInsertValue(sval, val, i);
    }

    // Create a function that receives the parameter struct and then calls the actual function.
    auto llvm_func = llvmFunction(func);
    auto name = llvm_func->getName().str();

    llvm::Function* lfunc = nullptr;

    llvm::Value* f = lookupCachedValue("fiber-func", name);

    if ( f )
        lfunc = llvm::cast<llvm::Function>(f);

    if ( ! lfunc) {
        llvm_parameter_list params = {
            std::make_pair("fiber", llvmTypePtr(llvmLibType("hlt.fiber"))),
            std::make_pair("fiber.args", llvmTypePtr(sty))
        };

        lfunc = llvmAddFunction(string(".fiber.run") + name, llvmTypeVoid(), params, true, type::function::C);

        pushFunction(lfunc);

        auto fiber = lfunc->arg_begin();
        auto fsval = builder()->CreateLoad(++lfunc->arg_begin());

        value_list cargs { fiber };
        _functions.back()->context = llvmCallC("hlt_fiber_context", cargs, false, false);

        llvmProfilerStart("fiber/inner");

        expr_list fargs;

        for ( int i = 0; i < args.size(); ++i ) {
            auto val = llvmExtractValue(fsval, i);
            fargs.push_back(builder::codegen::create(args[i]->type(), val));
        }

        auto result = llvmDoCall(llvm_func, func, nullptr, ftype, fargs, result_cctored, nullptr, false);

        llvmProfilerStop("fiber/inner");

        _functions.back()->context = nullptr;

        if ( ! rtype->equal(builder::void_::type()) ) {
            value_list args = { fiber };
            llvm::Value* rptr = llvmCallC("hlt_fiber_get_result_ptr", args, false, false);
            rptr = builder()->CreateBitCast(rptr, llvmTypePtr(llvmType(rtype)));
            llvmCreateStore(result, rptr);
        }

        builder()->CreateRetVoid();

        popFunction();

        cacheValue("fiber-func", name, lfunc);
    }

    // Create the fiber and start it.

    llvmProfilerStart("fiber/create");

    auto tmp = llvmAddTmp("fiber.arg", sty, sval, true);
    auto funcp = builder()->CreateBitCast(lfunc, llvmTypePtr());
    auto svalp = builder()->CreateBitCast(tmp, llvmTypePtr());

    value_list cargs { funcp, llvmExecutionContext(), svalp, llvmExecutionContext() };
    auto fiber = llvmCallC("hlt_fiber_create", cargs, false, false);

    llvmProfilerStop("fiber/create");

    auto result = llvmFiberStart(fiber, rtype);

    return result;
}

llvm::Value* CodeGen::llvmFiberStart(llvm::Value* fiber, shared_ptr<Type> rtype)
{
    llvmProfilerStart("fiber/start");

    llvm::Value* rptr = 0;

    if ( ! rtype->equal(builder::void_::type()) ) {
        rptr = llvmAddTmp("fiber.result", llvmType(rtype), nullptr, true);
        auto rptr_casted = builder()->CreateBitCast(rptr, llvmTypePtr());
        llvmCallC("hlt_fiber_set_result_ptr", { fiber, rptr_casted }, false, false);
    }

    value_list cargs { fiber, llvmExecutionContext() };
    auto result = llvmCallC("hlt_fiber_start", cargs, false, false);

    auto is_null = llvmCreateIsNull(result);
    auto done = newBuilder("done");
    auto yield = newBuilder("yielded");

    llvmProfilerStop("fiber/start");

    llvmCreateCondBr(is_null, yield, done);

    pushBuilder(yield);

    CodeGen::value_list args = { fiber, llvmLocationString(Location::None), llvmExecutionContext() };
    auto excpt = llvmCallC("hlt_exception_new_yield", args, false, false);
    value_list eargs = { llvmExecutionContext(), excpt };
    llvmCallC("__hlt_context_set_exception", eargs, false, false);
    llvmCreateBr(done);
    popBuilder();

    pushBuilder(done);

    if ( rptr )
        return builder()->CreateLoad(rptr);

    else
        return nullptr;

    // Leave builder on stack.
}

void CodeGen::llvmFiberYield(llvm::Value* fiber, shared_ptr<Type> blockable_ty, llvm::Value* blockable_val)
{
    if ( blockable_ty ) {
        assert(typeInfo(blockable_ty)->blockable.size());

        auto objptr = llvmAddTmp("obj", blockable_val->getType(), blockable_val, true);
        CodeGen::value_list args = { llvmRtti(blockable_ty), builder()->CreateBitCast(objptr, llvmTypePtr()) } ;
        blockable_val = llvmCallC("__hlt_object_blockable", args, true, true);
    }
    else
        blockable_val = llvmConstNull(llvmTypePtr(llvmLibType("hlt.blockable")));

    CodeGen::value_list args1 = { llvmExecutionContext(), blockable_val };
    llvmCallC("__hlt_context_set_blockable", args1, false, false);

    llvmAdaptStackForSafepoint(true);

    CodeGen::value_list args2 = { fiber };
    llvmCallC("hlt_fiber_yield", args2, false, false);

    llvmAdaptStackForSafepoint(false);
}

llvm::Value* CodeGen::llvmCallableBind(shared_ptr<Hook> hook, const expr_list args, bool ref, bool excpt_check, bool deep_copy_args, bool cctor_callable)
{
    return llvmDoCallableBind(nullptr, hook, hook, hook->type(), args, ref, excpt_check, deep_copy_args, cctor_callable);
}

llvm::Value* CodeGen::llvmCallableBind(shared_ptr<Function> func, shared_ptr<type::Function> ftype, const expr_list args, bool ref, bool excpt_check, bool deep_copy_args, bool cctor_callable)
{
    return llvmDoCallableBind(llvmFunction(func), func, nullptr, func->type(), args, ref, true, deep_copy_args, cctor_callable);
}

llvm::Value* CodeGen::llvmCallableBind(llvm::Value* llvm_func_val, shared_ptr<type::Function> ftype, const expr_list args, bool ref, bool excpt_check, bool deep_copy_args, bool cctor_callable)
{
    return llvmDoCallableBind(llvm_func_val, nullptr, nullptr, ftype, args, ref, excpt_check, deep_copy_args, cctor_callable);
}

llvm::Value* CodeGen::llvmDoCallableBind(llvm::Value* llvm_func_val, shared_ptr<Function> func, shared_ptr<Hook> hook, shared_ptr<type::Function> ftype, const expr_list args, bool result_cctor, bool excpt_check, bool deep_copy_args, bool cctor_callable)
{
    auto llvm_func = llvm_func_val ? llvm::cast<llvm::Function>(llvm_func_val) : nullptr;
    auto result = ftype->result();

    type::function::parameter_list unbound_args;

    auto params = ftype->parameters();
    auto p = params.rbegin();

    for ( int i = 0; i < params.size() - args.size(); i++ )
        unbound_args.push_front(*p++);

    auto cty = llvm::cast<llvm::StructType>(llvmLibType("hlt.callable"));

    CodeGen::type_list stypes;

    for ( int i = 0; i < cty->getNumElements(); ++i )
        stypes.push_back(cty->getElementType(i));

    for ( auto a : args )
        stypes.push_back(llvmType(a->type()));

    auto name = llvm_func ? llvm_func->getName().str() : hook->id()->name();
    auto sty = llvm::cast<llvm::StructType>(llvmTypeStruct(::string(".callable.args") + name, stypes));

    // Now fill a new callable object with its values.
    auto callable_type = std::make_shared<type::Callable>(result, unbound_args);
    llvm::Value* c = llvmObjectNew(callable_type, sty, cctor_callable);
    llvm::Value* s = builder()->CreateLoad(c);
    auto func_val = llvmCallableMakeFuncs(llvm_func, func, hook, ftype, result_cctor, excpt_check, sty, name, unbound_args);
    func_val = builder()->CreateBitCast(func_val, stypes[1]); // FIXME: Not sure why we need this cast.
    s = llvmInsertValue(s, func_val, 1);

    auto arg_start = cty->getNumElements();

    for ( int i = 0; i < args.size(); ++i ) {
        auto val = llvmValue(args[i]);

        if ( deep_copy_args ) {
            auto ti = llvmRtti(args[i]->type());
            auto src = llvmCreateAlloca(val->getType());
            auto dst = llvmCreateAlloca(val->getType());
            auto src_casted = builder()->CreateBitCast(src, llvmTypePtr());
            auto dst_casted = builder()->CreateBitCast(dst, llvmTypePtr());

            llvmCreateStore(val, src);
            value_list vals = { dst_casted, ti, src_casted };
            llvmCallC("hlt_clone_deep", vals, true, true);
            val = builder()->CreateLoad(dst);
        }

        else
            llvmCctor(val, args[i]->type(), false, "callable.call");

        s = llvmInsertValue(s, val, arg_start + i);
    }

    llvmCreateStore(s, c);
    return builder()->CreateBitCast(c, llvmTypePtr(cty));
}

llvm::Value* CodeGen::llvmCallableMakeFuncs(llvm::Function* llvm_func, shared_ptr<Function> func, shared_ptr<Hook> hook, shared_ptr<type::Function> ftype, bool result_cctor, bool excpt_check, llvm::StructType* sty, const string& name, const type::function::parameter_list& unbound_args)
{
    llvm::Value* cached = lookupCachedValue("callable-func", name);

    if ( cached )
        return cached;

    auto rtype = ftype->result()->type();
    auto is_void = rtype->equal(builder::void_::type());
    auto cty = llvm::cast<llvm::StructType>(llvmLibType("hlt.callable"));
    auto arg_start = cty->getNumElements();

    llvm::Type* llvm_rtype = nullptr;

    if ( hook )
        llvm_rtype = llvmType(rtype);

    else {
        assert(llvm_func);
        llvm_rtype = llvm_func->getReturnType();
    }

    // Build the internal function that will later call the target.
    CodeGen::parameter_list params = { std::make_pair("callable", builder::reference::type(builder::callable::type(rtype))) };

    int cnt = 0;
    for ( auto t : unbound_args ) {
        auto id = ::util::fmt("__ub%d", ++cnt);
        params.push_back(std::make_pair(id, t->type()));
    }

    auto llvm_call_func = llvmAddFunction(string(".callable.run") + name, llvm_rtype, params, true, type::function::HILTI);

    pushFunction(llvm_call_func);

    llvmDebugPrint("hilti-flow", ::util::fmt("entering callable's run function for %s", name));

    auto callable = builder()->CreateBitCast(llvm_call_func->arg_begin(), llvmTypePtr(sty));
    callable = builder()->CreateLoad(callable);

    auto fparams = ftype->parameters();
    auto targ = fparams.begin();
    expr_list nargs;

    for ( auto i = 0; i < fparams.size() - unbound_args.size(); i++ ) {
        auto val = llvmExtractValue(callable, arg_start + i);
        nargs.push_back(builder::codegen::create((*targ++)->type(), val));
    }

    auto farg = llvm_call_func->arg_begin();

    for ( auto t : unbound_args )
        nargs.push_back(builder::codegen::create(t->type(), ++farg));

    auto result = hook && ! rtype->equal(builder::void_::type())
        ? llvmAddTmp("hook.rval", llvm_rtype, nullptr, true) : nullptr;

    if ( result ) {
        llvmDoCall(llvm_func, func, hook, ftype, nargs, result_cctor, result, excpt_check);
        result = builder()->CreateLoad(result);
    }

    else
        result = llvmDoCall(llvm_func, func, hook, ftype, nargs, result_cctor, nullptr, excpt_check);

    llvmDebugPrint("hilti-flow", ::util::fmt("leaving callable's run function for %s", name));

    // Don't call llvmReturn() here as it would create the normal function
    // return code and reref the result, which return.result will have
    // already done.
    if ( rtype->equal(builder::void_::type()) )
        builder()->CreateRetVoid();
    else
        builder()->CreateRet(result);

    popFunction();

    // Create a separate version to call from C code. If the signature gets
    // changed here, the protogen code needs to adapt as well.

    llvm_parameter_list cparams;

    cparams.push_back(std::make_pair("callable", llvmTypePtr(llvmLibType("hlt.callable"))));
    cparams.push_back(std::make_pair("target", llvmTypePtr()));

    cnt = 0;
    for ( auto t : unbound_args ) {
        auto id = ::util::fmt("__ub%d", ++cnt);
        // We pass them all by pointer here so that we can deal with any parameters.
        cparams.push_back(std::make_pair(id, llvmTypePtr(llvmType(t->type()))));
    }

    // Plus standard HILTI-C paramters.
    cparams.push_back(std::make_pair(symbols::ArgException, llvmTypePtr(llvmTypeExceptionPtr())));
    cparams.push_back(std::make_pair(symbols::ArgExecutionContext, llvmTypePtr(llvmTypeExecutionContext())));

    auto llvm_call_func_c = llvmAddFunction(string(".callable.run.c") + name,
                                            llvmType(builder::void_::type()), cparams, true, true);

    pushFunction(llvm_call_func_c);

    llvmDebugPrint("hilti-flow", ::util::fmt("entering callable's C wrapper for %s", name));

    llvmClearException();

    std::vector<llvm::Value*> fargs;
    auto a = llvm_call_func_c->arg_begin();

    auto arg_callable = a++;
    auto arg_target = a++;

    fargs.push_back(arg_callable);

    for ( int i = 0; i < unbound_args.size(); i++ ) {
        auto deref = builder()->CreateLoad(a++);
        fargs.push_back(deref);
    }

    auto arg_expt = a++;
    auto arg_ctx = a++;

    fargs.push_back(arg_ctx);

    auto c_result = llvmCreateCall(llvm_call_func, fargs);

    if ( ! is_void ) {
        // Transfer result over.
        auto casted = builder()->CreateBitCast(arg_target, llvmTypePtr(llvm_rtype));
        llvmGCAssign(casted, c_result, rtype, true, false);
    }

    // Transfer exception over.
    auto ctx_excpt = llvmCurrentException();
    llvmGCAssign(arg_expt, ctx_excpt, builder::reference::type(builder::exception::typeAny()), false);
    llvmClearException();

    llvmDebugPrint("hilti-flow", ::util::fmt("leaving callable's C wrapper for %s", name));
    builder()->CreateRetVoid();
    popFunction();

    // Build the internal function that will later dtor all the arguments in
    // the struct. This functions clones only the parameters, the runtime
    // does the rest.

    llvm::Constant* llvm_dtor_func = nullptr;

    if ( ftype->parameters().size() ) {
        CodeGen::llvm_parameter_list lparams = {
            std::make_pair("callable", llvmTypePtr(cty)),
            std::make_pair(codegen::symbols::ArgExecutionContext, llvmTypePtr(llvmTypeExecutionContext()))
        };

        auto dtor = llvmAddFunction(string(".callable.dtor") + name, llvmTypeVoid(), lparams, true, true);

        pushFunction(dtor);

        callable = builder()->CreateBitCast(dtor->arg_begin(), llvmTypePtr(sty));
        callable = builder()->CreateLoad(callable);

        auto targ = fparams.begin();

        for ( auto i = 0; i < fparams.size() - unbound_args.size(); i++ ) {
            auto val = llvmExtractValue(callable, arg_start + i);
            llvmDtor(val, (*targ++)->type(), false, "callable.run.dtor");
        }

        llvmReturn();
        popFunction();

        llvm_dtor_func = llvm::ConstantExpr::getBitCast(dtor, llvmTypePtr());
    }
    else
        llvm_dtor_func = llvmConstNull(llvmTypePtr());

    // Build the internal function that will init a cloned callable. This
    // functions clones only the parameters, the runtime does the rest.

    llvm::Constant* llvm_clone_init_func = nullptr;

    if ( ftype->parameters().size() ) {
        CodeGen::llvm_parameter_list lparams = {
            std::make_pair("callable", llvmTypePtr(cty)),
            std::make_pair("callable", llvmTypePtr(cty)),
            std::make_pair("cstate", llvmTypePtr()),
            std::make_pair("excpt", llvmTypePtr(llvmTypeExceptionPtr())),
            std::make_pair("ctx", llvmTypePtr(llvmTypeExecutionContext()))
            };

        auto clone_init = llvmAddFunction(string(".callable.clone_init.params") + name, llvmTypeVoid(), lparams, true, true);

        pushFunction(clone_init);

        auto a = clone_init->arg_begin();
        auto arg_dst = a++;
        auto arg_src = a++;
        auto arg_cstate = a++;
        auto arg_excpt = a++;
        auto arg_ctx = a++;

        auto src = builder()->CreateBitCast(arg_src, llvmTypePtr(sty));
        auto dst = builder()->CreateBitCast(arg_dst, llvmTypePtr(sty));

        auto targ = fparams.begin();

        for ( auto i = 0; i < fparams.size() - unbound_args.size(); i++ ) {
            auto zero = llvmGEPIdx(0);
            auto argidx = llvmGEPIdx(arg_start + i);
            auto src_param = builder()->CreateBitCast(llvmGEP(src, zero, argidx), llvmTypePtr());
            auto dst_param = builder()->CreateBitCast(llvmGEP(dst, zero, argidx), llvmTypePtr());
            value_list args = { dst_param, llvmRtti((*targ++)->type()), src_param, arg_cstate, arg_excpt, arg_ctx };
            llvmCallC("__hlt_clone", args, false, false);
        }

        llvmReturn();
        popFunction();

        llvm_clone_init_func = llvm::ConstantExpr::getBitCast(clone_init, llvmTypePtr());
    }

    else
        llvm_clone_init_func = llvmConstNull(llvmTypePtr());

    // Build the per-function object for this callable.

    auto ctyfunc = llvm::cast<llvm::StructType>(llvmLibType("hlt.callable.func"));
    auto ctyfuncval = llvmConstNull(ctyfunc);
    auto object_size = llvmSizeOf(sty);
    ctyfuncval = llvmConstInsertValue(ctyfuncval, llvm::ConstantExpr::getBitCast(llvm_call_func, llvmTypePtr()), 0);
    ctyfuncval = llvmConstInsertValue(ctyfuncval, llvm::ConstantExpr::getBitCast(llvm_call_func_c, llvmTypePtr()), 1);
    ctyfuncval = llvmConstInsertValue(ctyfuncval, llvm_dtor_func, 2);
    ctyfuncval = llvmConstInsertValue(ctyfuncval, llvm_clone_init_func, 3);
    ctyfuncval = llvmConstInsertValue(ctyfuncval, object_size, 4);

    auto ctyfuncglob = llvmAddConst("callable.func" + name, ctyfuncval);

    return cacheValue("callable-func", name, ctyfuncglob);
}

llvm::Value* CodeGen::llvmCallableRun(shared_ptr<type::Callable> cty, llvm::Value* callable, const expr_list unbound_args)
{
    value_list args = { callable };
    std::vector<llvm::Type*> types = { callable->getType() };

    auto params = cty->Function::parameters();
    auto p = params.begin();

    for ( auto a : unbound_args ) {
        auto v = llvmValue(a, (*p++)->type());
        args.push_back(v);
        types.push_back(v->getType());
    }

    args.push_back(llvmExecutionContext());
    types.push_back(llvmTypePtr(llvmTypeExecutionContext()));

    auto ftype = llvm::FunctionType::get(llvmType(cty->result()->type()), types, false);
    auto funcobj = llvmExtractValue(builder()->CreateLoad(callable), 1);
    funcobj = builder()->CreateBitCast(funcobj, llvmTypePtr(llvmLibType("hlt.callable.func")));  // FIXME: Not sure why we need this cast.
    auto func = llvmExtractValue(builder()->CreateLoad(funcobj), 0);
    func = builder()->CreateBitCast(func, llvmTypePtr(ftype));

    // Can't use the safer llvmCreateCall() here because we have casted a
    // generic pointer into oru function pointer.
    auto result = builder()->CreateCall(func, args);
    result->setCallingConv(llvm::CallingConv::Fast);

    llvmBuildInstructionCleanup();

    llvmCheckException();

    if ( cty->result()->type()->equal(shared_ptr<Type>(new type::Void())) )
        return nullptr;

    return result;
}

llvm::Value* CodeGen::llvmRunHook(shared_ptr<Hook> hook, const expr_list& args, llvm::Value* result, bool cctor_result)
{
    return llvmDoCall(nullptr, hook, hook, hook->type(), args, cctor_result, result, true);
}

llvm::Value* CodeGen::llvmDoCall(llvm::Value* llvm_func, shared_ptr<Function> func, shared_ptr<Hook> hook, shared_ptr<type::Function> ftype, const expr_list& args, bool cctor_result, llvm::Value* hook_result, bool excpt_check, call_exception_callback_t excpt_callback)
{
    bool cleanup_precall = false;
    bool result_is_cctored = false;
    std::vector<llvm::Value*> llvm_args;

    // Prepare return value according to calling convention.

    switch ( ftype->callingConvention() ) {
     case type::function::HILTI:
     case type::function::HOOK:
        break;

     case type::function::HILTI_C:
        result_is_cctored = ftype->attributes().has(attribute::REF);
        break;

     case type::function::C:
        result_is_cctored = ftype->attributes().has(attribute::REF);
        break;

     case type::function::CALLABLE:
        internalError("llvmDoCall doesn't do callables (yet?)");

     default:
        internalError("unknown calling convention in llvmCall");
    }

    // Prepare parameters according to calling convention.

    auto arg = args.begin();

    for ( auto p : ftype->parameters() ) {
        auto ptype = p->type();

        auto coerced = (*arg)->coerceTo(ptype);
        auto arg_type = coerced->type();

        switch ( ftype->callingConvention() ) {
         case type::function::HILTI:
         case type::function::HOOK: {
            // Can pass directly but need context.
            assert(! ast::isA<hilti::type::TypeType>(arg_type)); // Not supported.
            auto arg_value = llvmValue(coerced, ptype, false);
            llvm_args.push_back(arg_value);
            cleanup_precall = false;
            break;
         }

         case type::function::HILTI_C: {
             if ( ast::isA<hilti::type::TypeType>(arg_type) ) {
                 // Pass just RTTI for type arguments.
                 auto tty = ast::as<hilti::type::TypeType>((*arg)->type());
                 assert(tty);
                 auto rtti = llvmRtti(tty->typeType());
                 llvm_args.push_back(rtti);
                 break;
             }

             auto arg_value = llvmValue(coerced, ptype);

             if ( typeInfo(ptype)->pass_type_info ) {
                 auto rtti = llvmRtti(arg_type);
                 auto arg_llvm_type = llvmType(arg_type);

                 llvm_args.push_back(rtti);
                 auto tmp = llvmAddTmp("arg", arg_llvm_type, arg_value);
                 llvm_args.push_back(builder()->CreateBitCast(tmp, llvmTypePtr()));
                 break;
             }

             llvm_args.push_back(arg_value);

             break;
         }

         case type::function::C: {
            assert(! ast::isA<hilti::type::TypeType>(arg_type)); // Not supported.

            // Don't mess with arguments.
            auto arg_value = llvmValue(coerced, ptype);
            llvm_args.push_back(arg_value);
            break;
         }

         default:
            internalError("unknown calling convention in llvmCall");
        }

        ++arg;
    }

    // Add additional parameters our calling convention may need.

    llvm::Value* excpt = nullptr;
    auto cc = ftype->callingConvention();

    switch ( cc ) {
     case type::function::HILTI:
        llvm_args.push_back(llvmExecutionContext());
        break;

     case type::function::HOOK:
        llvm_args.push_back(llvmExecutionContext());

        if ( hook_result )
            llvm_args.push_back(hook_result);
        break;

     case type::function::HILTI_C: {
         if ( ! ftype->attributes().has(attribute::NOEXCEPTION) )
             excpt = llvmAddTmp("excpt", llvmTypeExceptionPtr(), 0, true);
         else
             excpt = llvmConstNull(llvmTypePtr(llvmTypeExceptionPtr()));

         llvm_args.push_back(excpt);
         llvm_args.push_back(llvmExecutionContext());
        break;
     }

     case type::function::C:
        break;

     default:
        internalError("unknown calling convention in llvmCall");
    }

    // If it's a hook, redirect the call to the function that the linker will
    // create.
    if ( hook )
        llvm_func = llvmFunctionHookRun(hook);

    // Apply calling convention.
    auto orig_args = llvm_args;

    if ( cleanup_precall && _functions.back()->dtors_after_call ) {
        llvmBuildInstructionCleanup();
        _functions.back()->dtors_after_call = false;
    }

    auto t = llvmAdaptFunctionArgs(ftype);

    // Adapt reference counting for locals in case we reach a safepoint
    // during the call.
    if ( ftype->mayTriggerSafepoint() )
         llvmAdaptStackForSafepoint(true);

    auto result = abi()->createCall(llvm_func, llvm_args, t.first, t.second, ftype->callingConvention());

    // Back to normal
    if ( ftype->mayTriggerSafepoint() )
         llvmAdaptStackForSafepoint(false);

    if ( ! cleanup_precall && _functions.back()->dtors_after_call ) {
        llvmBuildInstructionCleanup();
        _functions.back()->dtors_after_call = false;
    }

    if ( result_is_cctored && ! cctor_result )
        llvmDtor(result, ftype->result()->type(), false, "llvmDoCall");

    if ( ! result_is_cctored && cctor_result )
        llvmCctor(result, ftype->result()->type(), false, "llvmDoCall");

    excpt_callback(this);

    switch ( cc ) {
     case type::function::HILTI_C:
        if ( ! ftype->attributes().has(attribute::NOEXCEPTION) )
            llvmCheckCException(excpt, excpt_check);

        break;

     default:
        if ( excpt_check && ! ftype->attributes().has(attribute::NOEXCEPTION) )
            llvmCheckException();
        break;
    }

    return result;

}
llvm::Value* CodeGen::llvmCall(shared_ptr<Function> func, const expr_list args, bool cctor_result, bool excpt_check, call_exception_callback_t excpt_callback)
{
    return llvmDoCall(llvmFunction(func), func, nullptr, func->type(), args, cctor_result, nullptr, excpt_check, excpt_callback);
}

llvm::Value* CodeGen::llvmCall(const string& name, const expr_list args, bool cctor_result, bool excpt_check, call_exception_callback_t excpt_callback)
{
    auto id = shared_ptr<ID>(new ID(name));
    auto expr = _hilti_module->body()->scope()->lookupUnique(id);

    if ( ! expr )
        internalError(string("unknown function ") + id->name() + " in llvmCall()");

    if ( ! ast::isA<expression::Function>(expr) )
        internalError(string("ID ") + id->name() + " is not a function in llvmCall()");

    auto func = ast::as<expression::Function>(expr)->function();

    return llvmCall(func, args, cctor_result, excpt_check, excpt_callback);
}

llvm::Value* CodeGen::llvmExtractBits(llvm::Value* value, llvm::Value* low, llvm::Value* high)
{
    auto width = llvm::cast<llvm::IntegerType>(value->getType())->getBitWidth();

    auto bits = builder()->CreateSub(llvmConstInt(width, width), high);
    bits = builder()->CreateAdd(bits, low);
    bits = builder()->CreateSub(bits, llvmConstInt(1, width));

    auto mask = builder()->CreateLShr(llvmConstInt(-1, width), bits);

    value = builder()->CreateLShr(value, low);

    return builder()->CreateAnd(value, mask);
}

llvm::Value* CodeGen::llvmInsertBits(llvm::Value* value, llvm::Value* low, llvm::Value* high)
{
    value = builder()->CreateShl(value, low);

    auto width = llvm::cast<llvm::IntegerType>(value->getType())->getBitWidth();

    auto bits = builder()->CreateSub(llvmConstInt(width, width), high);
    bits = builder()->CreateSub(bits, llvmConstInt(1, width));
    auto mask = builder()->CreateLShr(llvmConstInt(-1, width), bits);

    return builder()->CreateAnd(value, mask);
}

llvm::Value* CodeGen::llvmLocationString(const Location& l)
{
    return llvmConstAsciizPtr(string(l).c_str());
}

llvm::Value* CodeGen::llvmCurrentLocation(const string& addl)
{
    string s = string(_stmt_builder->currentLocation());

    if ( addl.size() )
        s += " [" + addl + "]";

    return llvmConstAsciizPtr(s);
}

void CodeGen::llvmDestroy(llvm::Value* val, shared_ptr<Type> type, const string& location_addl)
{
    if ( auto rtype = ast::tryCast<type::Reference>(type) )
        type = rtype->argType();

    assert(type::hasTrait<type::trait::HeapType>(type));

    auto ti = typeInfo(type);

    if ( ti->obj_dtor.size() == 0 && ti->obj_dtor_func == 0 )
        return;

    auto loc = options().debug ? llvmCurrentLocation(string("llvmDestroy/") + location_addl) : llvmConstNull();

    value_list args;
    args.push_back(llvmRtti(type));
    args.push_back(builder()->CreateBitCast(val, llvmTypePtr()));
    args.push_back(builder()->CreateBitCast(loc, llvmTypePtr()));
    args.push_back(llvmExecutionContext());
    llvmCallC("__hlt_object_destroy", args, false, false);
}

void CodeGen::llvmDtor(llvm::Value* val, shared_ptr<Type> type, bool is_ptr, const string& location_addl)
{
    auto ti = typeInfo(type);

    if ( ti->dtor.size() == 0 && ti->dtor_func == 0 )
        return;

    // If we didn't get a pointer to the value, we need to create a tmp so
    // that we can take its address.
    if ( ! is_ptr ) {
        auto tmp = llvmAddTmp("gcobj", llvmType(type), val, false);
        val = tmp;
    }

    auto loc = options().debug ? llvmCurrentLocation(string("llvmDtor/") + location_addl) : llvmConstNull();

    value_list args;
    args.push_back(llvmRtti(type));
    args.push_back(builder()->CreateBitCast(val, llvmTypePtr()));
    args.push_back(builder()->CreateBitCast(loc, llvmTypePtr()));
    args.push_back(llvmExecutionContext());
    llvmCallC("__hlt_object_dtor", args, false, false);
}

void CodeGen::llvmCctor(llvm::Value* val, shared_ptr<Type> type, bool is_ptr, const string& location_addl)
{
    auto ti = typeInfo(type);

    if ( ti->cctor.size() == 0 && ti->cctor_func == 0 )
        return;

    // If we didn't get a pointer to the value, we need to create a tmp so
    // that we can take its address.
    if ( ! is_ptr ) {
        auto tmp = llvmAddTmp("gcobj", llvmType(type), val, false);
        val = tmp;
    }

    auto loc = options().debug ? llvmCurrentLocation(string("llvmCctor/") + location_addl) : llvmConstNull();

    value_list args;
    args.push_back(llvmRtti(type));
    args.push_back(builder()->CreateBitCast(val, llvmTypePtr()));
    args.push_back(builder()->CreateBitCast(loc, llvmTypePtr()));
    args.push_back(llvmExecutionContext());
    llvmCallC("__hlt_object_cctor", args, false, false);
}

void CodeGen::llvmGCAssign(llvm::Value* dst, llvm::Value* val, shared_ptr<Type> type, bool plusone, bool dtor_first)
{
    assert(type::hasTrait<type::trait::ValueType>(type));

    if ( dtor_first )
        llvmDtor(dst, type, true, "gc-assign");

    llvmCreateStore(val, dst);

    if ( ! plusone )
        llvmCctor(dst, type, true, "gc-assign");
}

void CodeGen::llvmGCClear(llvm::Value* addr, shared_ptr<Type> type, const string& tag)
{
    assert(type::hasTrait<type::trait::ValueType>(type));
    auto init_val = typeInfo(type)->init_val;
    llvmDtor(addr, type, true, string("gc-clear/") + tag);
    llvmCreateStore(init_val, addr);
}

void CodeGen::llvmDebugPrint(const string& stream, const string& msg)
{
    if ( ! options().debug )
        return;

    auto arg1 = llvmConstAsciizPtr(stream);
    auto arg2 = llvmConstAsciizPtr(msg);

    value_list args;
    args.push_back(arg1);
    args.push_back(arg2);

    llvmCallC("__hlt_debug_print", args, false, false);

#if 0
    builder::tuple::element_list elems;
    elems.push_back(builder::string::create(msg));

    auto arg1 = builder::string::create(stream);
    auto arg2 = builder::string::create("%s");
    auto arg3 = builder::tuple::create(elems);

    expr_list args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    llvmCall("hlt::debug_printf", args, false);
#endif
}

void CodeGen::llvmDebugPushIndent()
{
    if ( ! options().debug )
        return;

    value_list args = { llvmExecutionContext() };
    llvmCallC("__hlt_debug_push_indent", args, false, false);
}

void CodeGen::llvmDebugPopIndent()
{
    if ( ! options().debug )
        return;

    value_list args = { llvmExecutionContext() };
    llvmCallC("__hlt_debug_pop_indent", args, false, false);
}

void CodeGen::llvmDebugPrintString(const string& str)
{
    if ( ! options().debug )
        return;

    auto s = llvmConstAsciizPtr(str);
    value_list args = { s, llvmExecutionContext() };
    llvmCallC(llvmLibFunction("__hlt_debug_print_str"), args, false);
}

void CodeGen::llvmDebugPrintPointer(const string& prefix, llvm::Value* ptr)
{
    if ( ! options().debug )
        return;

    auto s = llvmConstAsciizPtr(prefix);
    auto p = builder()->CreateBitCast(ptr, llvmTypePtr());
    value_list args = { s, p, llvmExecutionContext() };
    llvmCallC(llvmLibFunction("__hlt_debug_print_ptr"), args, false);
}

void CodeGen::llvmDebugPrintObject(const string& prefix, llvm::Value* ptr, shared_ptr<Type> type)
{
    if ( ! options().debug )
        return;

    auto s = llvmConstAsciizPtr(prefix);
    auto p = builder()->CreateBitCast(ptr, llvmTypePtr());
    value_list args = { s, p, llvmRtti(type), llvmExecutionContext() };
    llvmCallC(llvmLibFunction("__hlt_debug_print_object"), args, false);
}

llvm::Value* CodeGen::llvmSwitchEnumConst(llvm::Value* op, const case_list& cases, bool result, const Location& l)
{
    assert (op->getType()->isStructTy());

    // First check whether the enum has a value at all.
    //
    // FIXME: Copied from enum.cc, should factor out.
    auto flags = llvmExtractValue(op, 0);
    auto bit = builder()->CreateAnd(flags, llvmConstInt(HLT_ENUM_HAS_VAL, 64));
    auto have_val = builder()->CreateICmpNE(bit, llvmConstInt(0, 64));

    auto no_val = newBuilder("switch-no-val");
    auto cont = newBuilder("switch-do");
    llvmCreateCondBr(have_val, cont, no_val);

    pushBuilder(no_val);
    llvmRaiseException("Hilti::ValueError", l);
    popBuilder();

    pushBuilder(cont);
    auto switch_op = llvmExtractValue(op, 1);

    case_list ncases;

    for ( auto c : cases ) {
        assert(c._enums);

        std::list<llvm::ConstantInt*> nops;

        for ( auto op : c.op_enums ) {
            auto sval = llvm::cast<llvm::ConstantStruct>(op);
            auto val = llvmConstExtractValue(sval, 1);
            auto ival = llvm::cast<llvm::ConstantInt>(val);

            nops.push_back(llvmConstInt(ival->getZExtValue(), 64));
        }

        SwitchCase nc(c.label, nops, c.callback);
        ncases.push_back(nc);
    }

    return llvmSwitch(switch_op, ncases, result, l);
}

llvm::Value* CodeGen::llvmSwitch(llvm::Value* op, const case_list& cases, bool result, const Location& l)
{
    case_list ncases;
    const case_list* cases_to_use = &cases;

    assert(llvm::cast<llvm::IntegerType>(op->getType()));

    // If op is a constant, we prefilter the case list to directly remove all
    // cases that aren't matching.
    auto ci = llvm::dyn_cast<llvm::ConstantInt>(op);

    if ( ci ) {
        for ( auto c : cases ) {
            for ( auto op : c.op_integers ) {
                if ( llvm::cast<llvm::ConstantInt>(op)->getValue() == ci->getValue() ) {
                    ncases.push_back(c);
                    break;
                }
            }
        }

        cases_to_use = &ncases;
    }

    auto def = newBuilder("switch-default");
    auto cont = newBuilder("after-switch");
    auto switch_ = builder()->CreateSwitch(op, def->GetInsertBlock());

    std::list<std::pair<llvm::Value*, llvm::BasicBlock*>> returns;

    for ( auto c : *cases_to_use ) {
        assert(! c._enums);
        auto b = pushBuilder(::util::fmt("switch-%s", c.label.c_str()));
        auto r = c.callback(this);

        returns.push_back(std::make_pair(r, builder()->GetInsertBlock()));

        llvmCreateBr(cont);
        popBuilder();

        for ( auto op : c.op_integers )
            switch_->addCase(op, b->GetInsertBlock());
    }

    pushBuilder(def);
    llvmRaiseException("Hilti::ValueError", l);
    popBuilder();

    pushBuilder(cont); // Leave on stack.

    if ( ! result)
        return nullptr;

    assert(returns.size());

    auto phi = builder()->CreatePHI(returns.front().first->getType(), returns.size());

    for ( auto r : returns )
        phi->addIncoming(r.first, r.second);

    return phi;
}

static std::pair<int, shared_ptr<type::struct_::Field>> _getField(CodeGen* cg, shared_ptr<Type> type, const string& field)
{
    auto stype = ast::as<type::Struct>(type);

    if ( ! stype )
        cg->internalError("type is not a struct in _getField", type->location());

    int i = 0;

    for ( auto f : stype->fields() ) {
        if ( f->id()->name() == field )
            return std::make_pair(i, f);

        ++i;
    }

    cg->internalError(::util::fmt("unknown struct field name '%s' in _getField", field.c_str()), type->location());

    // Won't be reached.
    return std::make_pair(0, nullptr);
}

llvm::Value* CodeGen::llvmStructNew(shared_ptr<Type> type, bool ref)
{
    auto stype = ast::as <type::Struct>(type);
    auto llvm_stype = llvmType(stype);

    if ( ! stype->fields().size() )
        // Empty struct are ok, we turn then into null pointers.
        return llvmConstNull(llvm_stype);

    auto s = llvmObjectNew(stype, llvm::cast<llvm::StructType>(llvm::cast<llvm::PointerType>(llvm_stype)->getElementType()), ref);

    // Initialize fields
    auto zero = llvmGEPIdx(0);
    auto mask = 0;

    int j = 0;

    for ( auto f : stype->fields() ) {

        auto addr = llvmGEP(s, zero, llvmGEPIdx(j + 2));

        if ( f->default_() ) {
            // Initialize with default.
            mask |= (1 << j);
            auto llvm_default = llvmValue(f->default_(), f->type(), true);
            llvmGCAssign(addr, llvm_default, f->type(), true);
        }
        else
            // Initialize with null although we'll never access it. Better
            // safe than sorry ...
            llvmCreateStore(llvmConstNull(llvmType(f->type())), addr);

        ++j;
    }

    // Set mask.
    auto addr = llvmGEP(s, zero, llvmGEPIdx(1));
    llvmCreateStore(llvmConstInt(mask, 32), addr);

    return s;
}

llvm::Value* CodeGen::llvmStructGet(shared_ptr<Type> stype, llvm::Value* sval, int field, struct_get_default_callback_t default_, struct_get_filter_callback_t filter, const Location& l)
{
    auto i = ast::as<type::Struct>(stype)->fields().begin();
    std::advance(i, field);
    return llvmStructGet(stype, sval, (*i)->id()->name(), default_, filter, l);
}

llvm::Value* CodeGen::llvmStructGet(shared_ptr<Type> stype, llvm::Value* sval, const string& field, struct_get_default_callback_t default_, struct_get_filter_callback_t filter, const Location& l)
{
    auto fd = _getField(this, stype, field);
    auto idx = fd.first;
    auto f = fd.second;

    // Check whether field is set.
    auto zero = llvmGEPIdx(0);
    auto addr = llvmGEP(sval, zero, llvmGEPIdx(1));
    auto mask = builder()->CreateLoad(addr);

    auto bit = llvmConstInt(1 << idx, 32);
    auto isset = builder()->CreateAnd(bit, mask);

    auto block_ok = newBuilder("ok");
    auto block_not_set = newBuilder("not_set");
    auto block_done = newBuilder("done");
    IRBuilder* ok_exit = block_ok;

    auto notzero = builder()->CreateICmpNE(isset, llvmConstInt(0, 32));
    llvmCreateCondBr(notzero, block_ok, block_not_set);

    pushBuilder(block_ok);

    // Load field
    addr = llvmGEP(sval, zero, llvmGEPIdx(idx + 2));
    llvm::Value* result_ok = builder()->CreateLoad(addr);

    if ( filter ) {
        result_ok = filter(this, result_ok);
        ok_exit = builder();
    }

    llvmCreateBr(block_done);
    popBuilder();

    pushBuilder(block_not_set);

    llvm::Value* def = nullptr;

    // Unset, raise exception if no default.

    if ( ! default_ )
        llvmRaiseException("Hilti::UndefinedValue", l);
    else
        def = default_(this);

    llvmCreateBr(block_done);
    popBuilder();

    pushBuilder(block_done);

    llvm::Value* result = nullptr;

    if ( default_ ) {
        auto phi = builder()->CreatePHI(result_ok->getType(), 2);
        phi->addIncoming(result_ok, ok_exit->GetInsertBlock());
        phi->addIncoming(def, block_not_set->GetInsertBlock());
        result = phi;
    }

    else
        result = result_ok;

    // Leave builder on stack.

    return result;
}

void CodeGen::llvmStructSet(shared_ptr<Type> stype, llvm::Value* sval, int field, shared_ptr<Expression> val)
{
    auto i = ast::as<type::Struct>(stype)->fields().begin();
    std::advance(i, field);
    return llvmStructSet(stype, sval, (*i)->id()->name(), val);
}

void CodeGen::llvmStructSet(shared_ptr<Type> stype, llvm::Value* sval, const string& field, shared_ptr<Expression> val)
{
    auto fd = _getField(this, stype, field);
    auto cval = val->coerceTo(fd.second->type());
    auto lval = llvmValue(val, fd.second->type());
    llvmStructSet(stype, sval, field, lval);
}

void CodeGen::llvmStructSet(shared_ptr<Type> stype, llvm::Value* sval, int field, llvm::Value* val)
{
    auto i = ast::as<type::Struct>(stype)->fields().begin();
    std::advance(i, field);
    return llvmStructSet(stype, sval, (*i)->id()->name(), val);
}

void CodeGen::llvmStructSet(shared_ptr<Type> stype, llvm::Value* sval, const string& field, llvm::Value* val)
{
    auto fd = _getField(this, stype, field);
    auto idx = fd.first;
    auto f = fd.second;

    // Set mask bit.
    auto zero = llvmGEPIdx(0);
    auto addr = llvmGEP(sval, zero, llvmGEPIdx(1));
    auto mask = builder()->CreateLoad(addr);
    auto bit = llvmConstInt(1 << idx, 32);
    auto new_ = builder()->CreateOr(bit, mask);
    llvmCreateStore(new_, addr);

    addr = llvmGEP(sval, zero, llvmGEPIdx(idx + 2));
    llvmGCAssign(addr, val, f->type(), false);
}

void CodeGen::llvmStructUnset(shared_ptr<Type> stype, llvm::Value* sval, int field)
{
    auto i = ast::as<type::Struct>(stype)->fields().begin();
    std::advance(i, field);
    return llvmStructUnset(stype, sval, (*i)->id()->name());
}

void CodeGen::llvmStructUnset(shared_ptr<Type> stype, llvm::Value* sval, const string& field)
{
    auto fd = _getField(this, stype, field);
    auto idx = fd.first;
    auto f = fd.second;

    // Clear mask bit.
    auto zero = llvmGEPIdx(0);
    auto addr = llvmGEP(sval, zero, llvmGEPIdx(1));
    auto mask = builder()->CreateLoad(addr);
    auto bit = llvmConstInt(~(1 << idx), 32);
    auto new_ = builder()->CreateAnd(bit, mask);
    llvmCreateStore(new_, addr);

    addr = llvmGEP(sval, zero, llvmGEPIdx(idx + 2));
    llvmGCClear(addr, f->type(), "struct-unset");
}

llvm::Value* CodeGen::llvmStructIsSet(shared_ptr<Type> stype, llvm::Value* sval, const string& field)
{
    auto fd = _getField(this, stype, field);
    auto idx = fd.first;
    auto f = fd.second;

    // Check mask.
    auto zero = llvmGEPIdx(0);
    auto addr = llvmGEP(sval, zero, llvmGEPIdx(1));
    auto mask = builder()->CreateLoad(addr);
    auto bit = llvmConstInt(1 << idx, 32);
    auto isset = builder()->CreateAnd(bit, mask);
    auto notzero = builder()->CreateICmpNE(isset, llvmConstInt(0, 32));

    return notzero;
}

llvm::Value* CodeGen::llvmTupleElement(shared_ptr<Type> type, llvm::Value* tval, int idx, bool cctor)
{
    auto ttype = ast::as<type::Tuple>(type);
    assert(ttype);

    // ttype can be null if cctor is false.
    assert(ttype || ! cctor);

    shared_ptr<Type> elem_type;

    if ( ttype ) {
        int i = 0;
        for ( auto t : ttype->typeList() ) {
            if ( i++ == idx )
                elem_type = t;
        }
    }

    auto result = llvmExtractValue(tval, idx);

    if ( cctor )
        llvmCctor(result, elem_type, false, "tuple-element");

    return result;
}

llvm::Value* CodeGen::llvmIterBytesEnd()
{
    return llvmConstNull(llvmLibType("hlt.iterator.bytes"));
}

llvm::Value* CodeGen::llvmMalloc(llvm::Type* ty, const string& type, const Location& l)
{
    value_list args { llvmSizeOf(ty), llvmConstAsciizPtr(type), llvmConstAsciizPtr(l) };
    auto result = llvmCallC("__hlt_malloc", args, false, false);
    return builder()->CreateBitCast(result, llvmTypePtr(ty));
}

llvm::Value* CodeGen::llvmMalloc(llvm::Value* size, const string& type, const Location& l)
{
    value_list args { builder()->CreateZExt(size, llvmTypeInt(64)), llvmConstAsciizPtr(type), llvmConstAsciizPtr(l) };
    auto result = llvmCallC("__hlt_malloc", args, false, false);
    return builder()->CreateBitCast(result, llvmTypePtr());
}

void CodeGen::llvmFree(llvm::Value* val, const string& type, const Location& l)
{
    val = builder()->CreateBitCast(val, llvmTypePtr());
    value_list args { val, llvmConstAsciizPtr(type), llvmConstAsciizPtr(l) };
    llvmCallC("__hlt_free", args, false, false);
}

llvm::Value* CodeGen::llvmObjectNew(shared_ptr<Type> type, llvm::StructType* llvm_type, bool ref)
{
    value_list args {
        llvmRtti(type),
        llvmSizeOf(llvm_type),
        llvmConstAsciizPtr("llvm.object.new"),
        llvmExecutionContext()
        };

    auto func = ref ? "__hlt_object_new_ref" : "__hlt_object_new";
    auto result = llvmCallC(func, args, false, false);
    return builder()->CreateBitCast(result, llvmTypePtr(llvm_type));
}

shared_ptr<Type> CodeGen::typeByName(const string& name)
{
    auto expr = _hilti_module->body()->scope()->lookupUnique(std::make_shared<ID>(name));

    if ( ! expr )
        internalError(string("unknown type ") + name + " in typeByName()");

    if ( ! ast::isA<expression::Type>(expr) )
        internalError(string("ID ") + name + " is not a type in typeByName()");

    return ast::as<expression::Type>(expr)->typeValue();
}

   /// Creates a tuple from a givem list of elements. The returned tuple will
   /// have the cctor called for all its members.
   ///
   /// elems: The tuple elements.
   ///
   /// Returns: The new tuple.
llvm::Value* CodeGen::llvmTuple(shared_ptr<Type> type, const element_list& elems, bool cctor)
{
    auto ttype = ast::as<type::Tuple>(type);
    auto t = ttype->typeList().begin();

    value_list vals;

    for ( auto e : elems ) {
        auto op = builder::codegen::create(e.first, e.second);
        vals.push_back(llvmValue(op, *t++, cctor));
    }

    return llvmTuple(vals);
}

llvm::Value* CodeGen::llvmTuple(shared_ptr<Type> type, const expression_list& elems, bool cctor)
{
    auto ttype = ast::as<type::Tuple>(type);
    auto e = elems.begin();

    value_list vals;

    for ( auto t : ttype->typeList() )
        vals.push_back(llvmValue(*e++, t, cctor));

    return llvmTuple(vals);
}

llvm::Value* CodeGen::llvmTuple(const value_list& elems)
{
    return llvmValueStruct(elems);
}

llvm::Value* CodeGen::llvmClassifierField(shared_ptr<Type> field_type, shared_ptr<Type> src_type, llvm::Value* src_val, const Location& l)
{
    return _field_builder->llvmClassifierField(field_type, src_type, src_val, l);
}

llvm::Value* CodeGen::llvmClassifierField(llvm::Value* data, llvm::Value* len, llvm::Value* bits, const Location& l)
{
    auto ft = llvmLibType("hlt.classifier.field");

    llvm::Value* size = llvmSizeOf(ft);
    size = builder()->CreateAdd(size, builder()->CreateZExt(len, size->getType()));
    auto field = llvmMalloc(size, "hlt.classifier.field", l);
    field = builder()->CreateBitCast(field, llvmTypePtr(ft));

    if ( ! bits )
        bits = builder()->CreateMul(len, llvmConstInt(8, 64));

    // Initialize the static attributes of the field.
    llvm::Value* s = llvmConstNull(ft);
    s = llvmInsertValue(s, len, 0);
    s = llvmInsertValue(s, bits, 1);

    llvmCreateStore(s, field);

    // Copy the data bytes into the field.
    if ( data ) {
        data = builder()->CreateBitCast(data, llvmTypePtr());
        auto dst = llvmGEP(field, llvmGEPIdx(0), llvmGEPIdx(2));
        dst = builder()->CreateBitCast(dst, llvmTypePtr());
        llvmMemcpy(dst, data, len);
    }
    else {
        assert(len==0);
    }

    return builder()->CreateBitCast(field, llvmTypePtr());
}

llvm::Value* CodeGen::llvmHtoN(llvm::Value* val)
{
    auto itype = llvm::cast<llvm::IntegerType>(val->getType());
    assert(itype);

    int width = itype->getBitWidth();
    const char* f = "";

    switch ( width ) {
     case 8:
        return val;

     case 16:
        f = "hlt::hton16";
        break;

     case 32:
        f = "hlt::hton32";
        break;

     case 64:
        f = "hlt::hton64";
        break;

     default:
        internalError("unexpected bit width in llvmNtoH");
    }

    expr_list args = { builder::codegen::create(builder::integer::type(width), val) };
    return llvmCall(f, args, false);
}

llvm::Value* CodeGen::llvmNtoH(llvm::Value* val)
{
    auto itype = llvm::cast<llvm::IntegerType>(val->getType());
    assert(itype);

    int width = itype->getBitWidth();
    const char* f = "";

    switch ( width ) {
     case 8:
        return val;

     case 16:
        f = "hlt::ntoh16";
        break;

     case 32:
        f = "hlt::ntoh32";
        break;

     case 64:
        f = "hlt::ntoh64";
        break;

     default:
        internalError("unexpected bit width in llvmHtoN");
    }

    expr_list args = { builder::codegen::create(builder::integer::type(width), val) };
    return llvmCall(f, args, false);
}

void CodeGen::llvmMemcpy(llvm::Value *dst, llvm::Value *src, llvm::Value *n)
{
    src = builder()->CreateBitCast(src, llvmTypePtr());
    dst = builder()->CreateBitCast(dst, llvmTypePtr());
    n = builder()->CreateZExt(n, llvmTypeInt(64));

    CodeGen::value_list args = { dst, src, n, llvmConstInt(1, 32), llvmConstInt(0, 1) };
    std::vector<llvm::Type *> tys = { llvmTypePtr(), llvmTypePtr(), llvmTypeInt(64) };

    llvmCallIntrinsic(llvm::Intrinsic::memcpy, tys, args);
}

llvm::Value* CodeGen::llvmMemEqual(llvm::Value* p1, llvm::Value* p2, llvm::Value *n)
{
    p1 = builder()->CreateBitCast(p1, llvmTypePtr());
    p2 = builder()->CreateBitCast(p2, llvmTypePtr());
    n = builder()->CreateZExt(n, llvmTypeInt(64));

    CodeGen::value_list args = { p1, p2, n };
    auto result = llvmCallC("hlt_bcmp", args, false, false);
    return builder()->CreateTrunc(result, llvmTypeInt(1));
}


llvm::Value* CodeGen::llvmExpect(llvm::Value* v, llvm::Value* e)
{
    CodeGen::value_list args = { v, e };
    std::vector<llvm::Type *> tys = { v->getType() };
    return llvmCallIntrinsic(llvm::Intrinsic::expect, tys, args);
}

void CodeGen::llvmInstruction(shared_ptr<Instruction> instr, shared_ptr<Expression> op1, shared_ptr<Expression> op2, shared_ptr<Expression> op3, const Location& l)
{
    return llvmInstruction(nullptr, instr, op1, op2, op3, l);
}

void CodeGen::llvmInstruction(shared_ptr<Expression> target, shared_ptr<Instruction> instr, shared_ptr<Expression> op1, shared_ptr<Expression> op2, shared_ptr<Expression> op3, const Location& l)
{
    auto name = instr->id()->name();

    if ( ::util::startsWith(name, ".op.") ) {
        // These are dummy instructions used only to provide a single class
        // for the builder interface to access overloaded operators. We use
        // the non-prefixed name instead to do the lookup by name.
        name = name.substr(4, std::string::npos);
        return llvmInstruction(target, name, op1, op2, op3);
    }

    instruction::Operands ops = { target, op1, op2, op3 };
    auto id = std::make_shared<ID>(name);
    auto matches = InstructionRegistry::globalRegistry()->getMatching(id, ops);

    auto resolved = InstructionRegistry::globalRegistry()->resolveStatement(instr, ops);
    assert(resolved);

    _stmt_builder->llvmStatement(resolved, false);
}

void CodeGen::llvmInstruction(shared_ptr<Expression> target, const string& mnemo, shared_ptr<Expression> op1, shared_ptr<Expression> op2, shared_ptr<Expression> op3, const Location& l)
{
    instruction::Operands ops = { target, op1, op2, op3 };

    auto id = std::make_shared<ID>(mnemo);
    auto matches = InstructionRegistry::globalRegistry()->getMatching(id, ops);

    if ( matches.size() != 1 ) {
        fprintf(stderr, "target: %s\n", target ? target->type()->render().c_str() : "(null)");
        fprintf(stderr, "op1   : %s\n", op1 ? op1->type()->render().c_str() : "(null)");
        fprintf(stderr, "op2   : %s\n", op2 ? op2->type()->render().c_str() : "(null)");
        fprintf(stderr, "op3   : %s\n", op3 ? op3->type()->render().c_str() : "(null)");
        internalError(::util::fmt("llvmInstruction: %d matches for mnemo %s", matches.size(), mnemo.c_str()));
    }

    auto resolved = InstructionRegistry::globalRegistry()->resolveStatement(matches.front(), ops);
    assert(resolved);

    _stmt_builder->llvmStatement(resolved, false);
}

shared_ptr<hilti::Expression> CodeGen::makeLocal(const string& name, shared_ptr<Type> type, const AttributeSet& attrs)
{
    string n = "__" + name;

    int idx = 1;
    string unique_name = name;

    while ( _functions.back()->locals.find(unique_name) != _functions.back()->locals.end() )
        unique_name = ::util::fmt("%s.%d", n.c_str(), ++idx);

    llvmAddLocal(unique_name, type, nullptr, false);

    auto id = std::make_shared<ID>(unique_name);
    auto var = std::make_shared<variable::Local>(id, type);
    auto expr = std::make_shared<expression::Variable>(var);

    var->setInternalName(unique_name);

    return expr;
}

std::pair<bool, IRBuilder*> CodeGen::topEndOfBlockHandler()
{
    if ( ! _functions.back()->handle_block_end.size() )
        return std::make_pair(true, (IRBuilder*)nullptr);

    auto handler = _functions.back()->handle_block_end.back();
    return std::make_pair(handler != nullptr, handler);
}

void CodeGen::llvmBlockingInstruction(statement::Instruction* i, try_func try_, finish_func finish,
                                      shared_ptr<Type> blockable_ty, llvm::Value* blockable_val)
{
    auto loop = newBuilder("blocking-try");
    auto yield_ = newBuilder("blocking-yield");
    auto done = newBuilder("blocking-finish");

    llvmCreateBr(loop);

    pushBuilder(loop);
    auto result = try_(this, i);

    auto blocked = llvmMatchException("Hilti::WouldBlock", llvmCurrentException());
    llvmCreateCondBr(blocked, yield_, done);
    popBuilder();

    pushBuilder(yield_);
    llvmClearException();
    auto fiber = llvmCurrentFiber();
    llvmFiberYield(fiber, blockable_ty, blockable_val);
    llvmCreateBr(loop);
    popBuilder();

    pushBuilder(done);
    llvmCheckException();
    finish(this, i, result);

    // Leave on stack.
}

void CodeGen::llvmProfilerStart(llvm::Value* tag, llvm::Value* style, llvm::Value* param, llvm::Value* tmgr)
{
    assert(tag);

    if ( options().profile == 0 )
        return;

    if ( ! style )
        style = llvmEnum("Hilti::ProfileStyle::Standard");

    if ( ! param )
        param = llvmConstInt(0, 64);

    if ( ! tmgr ) {
        auto rtmgr = builder::reference::type(builder::timer_mgr::type());
        tmgr = llvmConstNull(llvmType(rtmgr));
    }

    auto eexpr = _hilti_module->body()->scope()->lookupUnique((std::make_shared<ID>("Hilti::ProfileStyle")));
    assert(eexpr);

    expr_list args = {
        builder::codegen::create(builder::string::type(), tag),
        builder::codegen::create(ast::checkedCast<expression::Type>(eexpr)->typeValue(), style),
        builder::codegen::create(builder::integer::type(64), param),
        builder::codegen::create(builder::reference::type(builder::timer_mgr::type()), tmgr)
    };

    llvmCall("hlt::profiler_start", args, false, false);
}

void CodeGen::llvmProfilerStart(const string& tag, const string& style, int64_t param, llvm::Value* tmgr)
{
    assert(tag.size());

    if ( options().profile == 0 )
        return;

    auto ltag = llvmStringFromData(tag);
    auto lstyle = style.size() ? llvmEnum(style) : static_cast<llvm::Value*>(nullptr);
    auto lparam = llvmConstInt(param, 64);

    llvmProfilerStart(ltag, lstyle, lparam, tmgr);
}

void CodeGen::llvmProfilerStop(llvm::Value* tag)
{
    assert(tag);

    if ( options().profile == 0 )
        return;

    expr_list args = {
        builder::codegen::create(builder::string::type(), tag),
    };

    llvmCall("hlt::profiler_stop", args, false, false);
}

void CodeGen::llvmProfilerStop(const string& tag)
{
    assert(tag.size());

    if ( options().profile == 0 )
        return;

    auto ltag = llvmStringFromData(tag);
    llvmProfilerStop(ltag);
}

void CodeGen::llvmProfilerUpdate(llvm::Value* tag, llvm::Value* arg)
{
    assert(tag);

    if ( options().profile == 0 )
        return;

    if ( ! arg )
        arg = llvmConstInt(0, 64);

    expr_list args = {
        builder::codegen::create(builder::string::type(), tag),
        builder::codegen::create(builder::integer::type(64), arg),
    };

    llvmCall("hlt::profiler_update", args, false, false);
}

void CodeGen::llvmProfilerUpdate(const string& tag, int64_t arg)
{
    assert(tag.size());

    auto ltag = llvmString(tag);
    auto larg = llvmConstInt(arg, 64);

    llvmProfilerUpdate(ltag, larg);
}

string CodeGen::llvmGetModuleIdentifier(llvm::Module* module)
{
    auto md = module->getNamedMetadata(symbols::MetaModuleName);

    if ( md ) {
        auto node = llvm::cast<llvm::MDNode>(md->getOperand(0));
        return llvm::cast<llvm::MDString>(node->getOperand(0))->getString();
    }

    return linkerModuleIdentifierStatic(module);
}

string CodeGen::linkerModuleIdentifier() const
{
    return linkerModuleIdentifierStatic(_module);
}

string CodeGen::linkerModuleIdentifierStatic(llvm::Module* module)
{
    // We add this pointer here so that diffent compile-time units can use
    // the name module name.
    return ::util::fmt("%s.%p", module->getModuleIdentifier(), module);
}

void CodeGen::prepareCall(shared_ptr<Expression> func, shared_ptr<Expression> args, CodeGen::expr_list* call_params, bool before_call)
{
    return _stmt_builder->prepareCall(func, args, call_params, before_call);
}
