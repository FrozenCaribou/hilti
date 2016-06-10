
#include "../module.h"
#include "codegen.h"
#include "abi.h"

#include "libffi/src/x86/ffi64.h"
#include <llvm-c/TargetMachine.h>

using namespace hilti;
using namespace codegen;

unique_ptr<ABI> ABI::createABI(CodeGen* cg)
{
    string striple = LLVMGetDefaultTargetTriple();
    llvm::Triple triple(striple);

    unique_ptr<ABI> abi;

    if ( triple.getArch() == llvm::Triple::x86_64 ) {
        abi::X86_64::Flavor flavor = abi::X86_64::FLAVOR_DEFAULT;

        if ( triple.isOSDarwin() )
            flavor = abi::X86_64::FLAVOR_DARWIN;

        abi = unique_ptr<ABI>(new abi::X86_64(flavor));
    }


    if ( ! abi ) {
        fprintf(stderr, "unsupported platform %s\n", striple.c_str());
        exit(1);
    }

    abi->_triple = striple;
    abi->_cg = cg;

    return abi;
}

ABI::ByteOrder ABI::byteOrder() const
{
    if ( llvm::sys::IsLittleEndianHost )
        return LittleEndian;

    if ( llvm::sys::IsBigEndianHost )
        return BigEndian;

    cg()->internalError("unknown endianess of target arch");
    assert(false);
    return BigEndian; // Cannot be reached but make compiler happy.
}

// Converts LLVM type into libffi's description.
static ffi_type* _llvmToCif(CodeGen* cg, llvm::Type* type)
{
    switch ( type->getTypeID() ) {

     case llvm::Type::VoidTyID:
        return &ffi_type_void;

     case llvm::Type::DoubleTyID:
        return &ffi_type_double;

     case llvm::Type::PointerTyID:
        return &ffi_type_pointer;

     case llvm::Type::IntegerTyID: {
         int w = llvm::cast<llvm::IntegerType>(type)->getBitWidth();

         if ( w <= 8 )
             return &ffi_type_sint8;

         if ( w <= 16 )
             return &ffi_type_sint16;

         if ( w <= 32 )
             return &ffi_type_sint32;

         if ( w <= 64 )
             return &ffi_type_sint64;

         cg->internalError("integer bitwidth >64 in llvmToCif");
         break;
     }

     case llvm::Type::StructTyID: {
         auto sty = llvm::cast<llvm::StructType>(type);
         int n = sty->getNumElements();

         ffi_type** elems_ffi = new ffi_type*[n+1];

         for ( int i = 0; i < n; i++ )
             elems_ffi[i] = _llvmToCif(cg, sty->getElementType(i));
         elems_ffi[n] = 0;

         auto cif = new ffi_type;
         cif->size = cif->alignment = 0;
         cif->type = FFI_TYPE_STRUCT;
         cif->elements = elems_ffi;

         return cif;
     }

     case llvm::Type::ArrayTyID: {
         auto sty = llvm::cast<llvm::ArrayType>(type);
         int n = sty->getNumElements();

         ffi_type** elems_ffi = new ffi_type*[n+1];

         for ( int i = 0; i < n; i++ )
             elems_ffi[i] = _llvmToCif(cg, sty->getElementType());

         elems_ffi[n] = 0;

         auto cif = new ffi_type;
         cif->size = cif->alignment = 0;
         cif->type = FFI_TYPE_STRUCT;
         cif->elements = elems_ffi;

         return cif;
    }

     case llvm::Type::HalfTyID:
     case llvm::Type::FloatTyID:
     case llvm::Type::X86_FP80TyID:
     case llvm::Type::FP128TyID:
     case llvm::Type::PPC_FP128TyID:
     case llvm::Type::LabelTyID:
     case llvm::Type::MetadataTyID:
     case llvm::Type::X86_MMXTyID:
     case llvm::Type::FunctionTyID:
     case llvm::Type::VectorTyID:
     default:
        cg->internalError(::util::fmt("unsupport argument type in llvmToCif: %s", type->getTypeID()));
    }

    assert(false); // cannot be reached.
    return 0;
}

ffi_cif* ABI::getCIF(llvm::Type* rtype, const argument_type_list& args)
{
    assert(rtype);

    auto rtype_ffi = _llvmToCif(cg(), rtype);

    auto nargs = args.size();

    ffi_type** args_ffi = new ffi_type*[nargs];

#if 0
    fprintf(stderr, "--\n");
    if ( rtype ) {
        rtype->dump();
        fprintf(stderr, " RESULT\n");
    }
#endif

    for ( int i = 0; i < nargs; ++i ) {
        args_ffi[i] = _llvmToCif(cg(), args[i]);
#if 0
        args[i]->dump();
        fprintf(stderr, "  %d\n", i);
#endif
    }

    auto cif = new ffi_cif;
    auto rc = ffi_prep_cif(cif, FFI_DEFAULT_ABI, nargs, rtype_ffi, args_ffi);
    assert(rc == FFI_OK);

    return cif;
}

// TODO: This function mimics a subpart of what libffi is doing. However, it
// remains unclear of this is sufficient: we just determine which arguments
// are passed in registers and which aren't, but we don't do the register
// assignment ourselves but hope that LLVM takes care of that correctly and
// in alignment with what the FFI code would do ...
abi::X86_64::ClassifiedArguments abi::X86_64::classifyArguments(const string& name, llvm::Type* rtype, const ABI::arg_list& args, type::function::CallingConvention cc)
{
    ClassifiedArguments cargs;

    std::vector<llvm::Type*> arg_types;

    for ( auto a : args )
        arg_types.push_back(a.second);

    auto cif = getCIF(rtype, arg_types);
    auto ffi_avn = cif->nargs;
    auto ffi_arg_types = cif->arg_types;

    assert(ffi_avn == args.size());

    // From ffi64.c.
    cargs.return_in_mem = (cif->rtype->type == FFI_TYPE_STRUCT && (cif->flags & 0xff) == FFI_TYPE_VOID);
    cargs.return_type = rtype;

    // The following logic follows ffi_call() in libffi/src/x86/ffi64.c.

    enum x86_64_reg_class classes[MAX_CLASSES];

    int gprcount = 0;
    int ssecount = 0;
    int ngpr = 0;
    int nsse = 0;

    // If the return value is passed in memory, that takes a register.
    if ( cargs.return_in_mem )
        ++gprcount;

    for ( int i = 0; i < args.size(); i++ ) {
        bool arg_in_mem = false;
        llvm::Type* new_llvm_type = args[i].second;

        int n = ffi64_examine_argument (ffi_arg_types[i], classes, 0, &ngpr, &nsse);

        // FIXME: The max register heuristic (which is copied from libffi)
        // doesn't seem to work. It kicks in when clang still doesn't pass
        // aggs.
        if ( n == 0 ) //|| gprcount + ngpr > MAX_GPR_REGS || ssecount + nsse > MAX_SSE_REGS )
            // Argument is passed in memory.
            arg_in_mem = true;

        else {
            // The argument is passed entirely in registers.
            for ( int j = 0; j < n; j++) {
                switch ( classes[j] ) {
                 case X86_64_INTEGER_CLASS:
                 case X86_64_INTEGERSI_CLASS:
                    gprcount++;
                    break;

                 case X86_64_SSE_CLASS:
                 case X86_64_SSEDF_CLASS:
                    ssecount++;
                    break;

                 case X86_64_SSESF_CLASS:
                    ssecount++;
                    break;

                 case X86_64_NO_CLASS:
                    break;

                 default:
                    fprintf(stderr, "unsupported register form: %d\n", classes[j]);
                    abort();
                }
            }
        }

        cargs.args_in_mem.push_back(arg_in_mem);
        cargs.arg_types.push_back(new_llvm_type);
    }

    if ( name.size() )
        _classified_arguments.insert(std::make_pair(name, cargs));

    return cargs;
}

abi::X86_64::ClassifiedArguments abi::X86_64::classifyArguments(const string& name)
{
    auto i = _classified_arguments.find(name);
    assert(i != _classified_arguments.end());
    return (*i).second;
}

/// X86_64 ABI.

#include "libffi/src/x86/ffi64.h"

llvm::Type* ABI::mapToIntType(llvm::StructType* stype)
{
    llvm::Type* rtype = 0;

    int i = 8 * cg()->llvmDataLayout()->getTypeAllocSize(stype);

    if ( i < 128 )
        // This condition is just a guess ...
        rtype = cg()->llvmTypeInt(i);

    // Leave null otherwise.

    return rtype;
}

llvm::FunctionType* abi::X86_64::createFunctionType(llvm::Type* rtype, const ABI::arg_list& args, type::function::CallingConvention cc)
{
    auto t = createFunctionTypeInternal(rtype, args, cc, "");
    return std::get<0>(t);
}

/// FIXME: We currently just generally pass structures in memory for HILTI
/// calling convention. For HILTI_C cc we leave them untouched.
std::tuple<llvm::FunctionType*, abi::X86_64::ClassifiedArguments, std::vector<string>, std::vector<int>, int>
abi::X86_64::createFunctionTypeInternal(llvm::Type* rtype, const ABI::arg_list& args, type::function::CallingConvention cc, const string& name)
{
    auto cargs = classifyArguments(name, rtype, args, cc);

    std::vector<llvm::Type*> ntypes;
    std::vector<string> nnames;
    std::vector<int> byvals;
    int arg_base = 0;

    if ( cargs.return_in_mem ) {
        // Move return type to new first paramter.
        ntypes.push_back(rtype->getPointerTo());
        nnames.push_back("agg.sret");
        rtype = llvm::Type::getVoidTy(cg()->llvmContext());
        arg_base = 1;
    }

    assert(cargs.args_in_mem.size() == args.size());

    for ( int i = 0; i < args.size(); i++ ) {
        if ( cargs.args_in_mem[i] ) {
            byvals.push_back(i);
            ntypes.push_back(cargs.arg_types[i]->getPointerTo());
            nnames.push_back(args[i].first);
        }

        else {
            auto t = cargs.arg_types[i];

            auto stype = llvm::dyn_cast<llvm::StructType>(t);
            if ( stype ) {
                auto itype = mapToIntType(stype);
                if ( itype )
                    t = itype;
            }

            ntypes.push_back(t);
            nnames.push_back(args[i].first);
        }
    }

    auto stype = llvm::dyn_cast<llvm::StructType>(cargs.return_type);

    if ( stype && ! cargs.return_in_mem ) {
        auto itype = mapToIntType(stype);
        if ( itype )
            rtype = itype;
    }

    auto ftype = llvm::FunctionType::get(rtype, ntypes, false);
    return std::make_tuple(ftype, cargs, nnames, byvals, arg_base);
}

llvm::Function* abi::X86_64::createFunction(const string& name, llvm::Type* rtype, const ABI::arg_list& args, llvm::GlobalValue::LinkageTypes linkage, llvm::Module* module, type::function::CallingConvention cc)
{
    auto t = createFunctionTypeInternal(rtype, args, cc, name);
    auto ftype = std::get<0>(t);
    auto cargs = std::get<1>(t);
    auto nnames = std::get<2>(t);
    auto byvals = std::get<3>(t);
    auto arg_base = std::get<4>(t);

    auto func = llvm::Function::Create(ftype, linkage, name, module);

    if ( cargs.return_in_mem ) {
        func->addAttribute(1, llvm::Attribute::StructRet);
        func->addAttribute(1, llvm::Attribute::NoAlias);
    }

    for ( auto i : byvals ) {
        func->addAttribute(i + 1 + arg_base, llvm::Attribute::ByVal);
        func->addAttribute(i + 1 + arg_base, llvm::Attribute::NoAlias);
    }

    auto i = nnames.begin();
    for ( auto a = func->arg_begin(); a != func->arg_end(); ++a )
        a->setName(*i++);

    return func;
}

llvm::Value* abi::X86_64::createCall(llvm::Value *callee, std::vector<llvm::Value *> args, llvm::Type* rtype, const arg_list& targs, type::function::CallingConvention cc)
{
    auto cargs = classifyArguments("", rtype, targs, cc);

    std::vector<llvm::Value*> nargs;

    llvm::Value* agg_ret = nullptr;

    if ( cargs.return_in_mem ) {
        // Add initial parameter for return value.
        agg_ret = cg()->llvmAddTmp("agg.sret", cargs.return_type, nullptr, false, 8);
        nargs.push_back(agg_ret);
    }

    assert(cargs.args_in_mem.size() == args.size());

    for ( int i = 0; i < args.size(); i++ ) {
        auto llvm_val = args[i];
        auto llvm_type = cargs.arg_types[i];

        if ( cargs.args_in_mem[i] ) {
            auto agg = cg()->llvmAddTmp("agg.arg", llvm_type, llvm_val, false, 8);
            nargs.push_back(agg);
        }

        else {
            auto stype = llvm::dyn_cast<llvm::StructType>(llvm_type);
            if ( stype ) {
                auto itype = mapToIntType(stype);
                if ( itype ) {
                    auto tmp = cg()->llvmCreateAlloca(llvm_type);
                    cg()->llvmCreateStore(llvm_val, tmp);
                    auto casted = cg()->builder()->CreateBitCast(tmp, cg()->llvmTypePtr(itype));
                    llvm_val = cg()->builder()->CreateLoad(casted);
                }
            }

            nargs.push_back(llvm_val);
        }
    }

    auto ci = cg()->llvmCreateCall(callee, nargs);
    auto llvm_cc = cg()->llvmCallingConvention(cc);
    ci->setCallingConv(llvm_cc);

    llvm::Value* result = ci;

    if ( cargs.return_in_mem )
        result = cg()->builder()->CreateLoad(agg_ret);

    else {
        auto stype = llvm::dyn_cast<llvm::StructType>(cargs.return_type);

        if ( stype ) {
            auto itype = mapToIntType(stype);
            if ( itype ) {
                auto tmp = cg()->llvmCreateAlloca(itype);
                cg()->llvmCreateStore(result, tmp);
                auto casted = cg()->builder()->CreateBitCast(tmp, cg()->llvmTypePtr(stype));
                result = cg()->builder()->CreateLoad(casted);
            }
        }
    }

    return result;
}

string abi::X86_64::dataLayout() const
{
#ifdef HAVE_LLVM_35
    // From clang.
    return "e-m:e-i64:64-f80:128-n8:16:32:64-S128";
#else
    return "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128";
#endif
}
