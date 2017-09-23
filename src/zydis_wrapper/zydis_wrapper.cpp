#include "zydis_wrapper.h"
#include <windows.h>

bool Zydis::mInitialized = false;
ZydisDecoder Zydis::mZyDecoder;
ZydisFormatter Zydis::mZyFormatter;

void Zydis::GlobalInitialize()
{
    if(!mInitialized)
    {
        mInitialized = true;
#ifdef _WIN64
		ZydisDecoderInit(&mZyDecoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
#else //x86
		ZydisDecoderInit(&mZyDecoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_ADDRESS_WIDTH_32);
#endif //_WIN64
		ZydisFormatterInit(&mZyFormatter, ZYDIS_FORMATTER_STYLE_INTEL);
    }
}

void Zydis::GlobalFinalize()
{
    mInitialized = false;
}

Zydis::Zydis()
    : mSuccess(false),
      mExplicitOpCount(0)
{
    GlobalInitialize();
}

Zydis::~Zydis()
{    
}

bool Zydis::Disassemble(size_t addr, const unsigned char data[MAX_DISASM_BUFFER])
{
    return Disassemble(addr, data, MAX_DISASM_BUFFER);
}

bool Zydis::Disassemble(size_t addr, const unsigned char* data, int size)
{
    if(!data || !size)
        return false;

    mSuccess = false;

    // Decode instruction.
	if(!ZYDIS_SUCCESS(ZydisDecoderDecodeBuffer(
		&mZyDecoder, data, size, addr, &mZyInstr
	)))
        return false;

    // Format it to human readable representation.
    if(!ZYDIS_SUCCESS(ZydisFormatterFormatInstruction(
        &mZyFormatter,
        const_cast<ZydisDecodedInstruction*>(&mZyInstr),
        mInstrText,
        sizeof(mInstrText)
    )))
        return false;

    // Count explicit operands.
    mExplicitOpCount = 0;
    for(size_t i = 0; i < ZYDIS_MAX_OPERAND_COUNT; ++i)
    {
        auto & op = mZyInstr.operands[i];

        // HACK (ath): Rebase IMM if relative (codebase expects it this way)
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.isRelative)
            ZydisUtilsCalcAbsoluteTargetAddress(&mZyInstr, &op, &op.imm.value.u);

        if (op.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
            break;

        ++mExplicitOpCount;
    }

    mSuccess = true;
    return true;
}

bool Zydis::DisassembleSafe(size_t addr, const unsigned char* data, int size)
{
    unsigned char dataSafe[MAX_DISASM_BUFFER];
    memset(dataSafe, 0, sizeof(dataSafe));
    memcpy(dataSafe, data, min(MAX_DISASM_BUFFER, size_t(size)));
    return Disassemble(addr, dataSafe);
}

const ZydisDecodedInstruction* Zydis::GetInstr() const
{
    if(!Success())
        return nullptr;
    return &mZyInstr;
}

bool Zydis::Success() const
{
    return mSuccess;
}

const char* Zydis::RegName(ZydisRegister reg) const
{
    switch(reg)
    {
    case ZYDIS_REGISTER_ST0:
        return "st(0)";
    case ZYDIS_REGISTER_ST1:
        return "st(1)";
    case ZYDIS_REGISTER_ST2:
        return "st(2)";
    case ZYDIS_REGISTER_ST3:
        return "st(3)";
    case ZYDIS_REGISTER_ST4:
        return "st(4)";
    case ZYDIS_REGISTER_ST5:
        return "st(5)";
    case ZYDIS_REGISTER_ST6:
        return "st(6)";
    case ZYDIS_REGISTER_ST7:
        return "st(7)";
    default:
		return ZydisRegisterGetString(reg);
    }
}

std::string Zydis::OperandText(int opindex) const
{
    if(!Success() || opindex >= mZyInstr.operandCount)
        return "";

    auto& op = mZyInstr.operands[opindex];

	ZydisFormatterHookType type;
	switch (op.type)
	{
	case ZYDIS_OPERAND_TYPE_IMMEDIATE:
		type = ZYDIS_FORMATTER_HOOK_FORMAT_OPERAND_IMM; break;
	case ZYDIS_OPERAND_TYPE_MEMORY:
		type = ZYDIS_FORMATTER_HOOK_FORMAT_OPERAND_MEM; break;
	case ZYDIS_OPERAND_TYPE_REGISTER:
		type = ZYDIS_FORMATTER_HOOK_FORMAT_OPERAND_REG; break;
	case ZYDIS_OPERAND_TYPE_POINTER:
		type = ZYDIS_FORMATTER_HOOK_FORMAT_OPERAND_PTR; break;
	default:
		return "";
	}

	ZydisFormatterFormatOperandFunc fmtFunc = nullptr;
	if (!ZYDIS_SUCCESS(ZydisFormatterSetHook(&mZyFormatter, type, (const void**)&fmtFunc)))
        return "";

	char buf[200] = "";
	auto bufPtr = buf;
	fmtFunc(
		&mZyFormatter, 
		&bufPtr, 
		sizeof(buf), 
		(ZydisDecodedInstruction*)&mZyInstr, 
		(ZydisDecodedOperand*)&op
	);
    
    return buf;
}

int Zydis::Size() const
{
    if(!Success())
        return 1;
    return GetInstr()->length;
}

size_t Zydis::Address() const
{
    if(!Success())
        return 0;

    return size_t(GetInstr()->instrAddress);
}

bool Zydis::IsFilling() const
{
    if(!Success())
        return false;

    switch(mZyInstr.mnemonic)
    {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_INT3:
        return true;
    default:
        return false;
    }
}

bool Zydis::IsBranchType(std::underlying_type_t<BranchType> bt) const
{
    if(!Success())
        return false;

    std::underlying_type_t<BranchType> ref = 0;
    const auto & op0 = mZyInstr.operands[0];

    switch(mZyInstr.mnemonic)
    {
    case ZYDIS_MNEMONIC_RET: 
        ref = BT_Ret; 
        break;
    case ZYDIS_MNEMONIC_CALL:
        ref = (op0.elementType == ZYDIS_ELEMENT_TYPE_STRUCT ||
            op0.type == ZYDIS_OPERAND_TYPE_POINTER) ? BT_FarCall : BT_Call;
        break;
    case ZYDIS_MNEMONIC_JMP:
        ref = (op0.elementType == ZYDIS_ELEMENT_TYPE_STRUCT ||
            op0.type == ZYDIS_OPERAND_TYPE_POINTER) ? BT_FarJmp : BT_UncondJmp;
        break;
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_JCXZ:
    case ZYDIS_MNEMONIC_JECXZ:
    case ZYDIS_MNEMONIC_JKNZD:
    case ZYDIS_MNEMONIC_JKZD:
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JZ:
        ref = BT_CondJmp;
        break;
    case ZYDIS_MNEMONIC_SYSCALL:
    case ZYDIS_MNEMONIC_SYSENTER:
        ref = BT_Syscall;
        break;
    case ZYDIS_MNEMONIC_SYSRET:
    case ZYDIS_MNEMONIC_SYSEXIT:
        ref = BT_Sysret;
        break;
    case ZYDIS_MNEMONIC_INT:
        ref = BT_Int;
        break;
    case ZYDIS_MNEMONIC_INT3:
        ref = BT_Int3;
        break;
    case ZYDIS_MNEMONIC_INT1:
        ref = BT_Int1;
        break;
    case ZYDIS_MNEMONIC_XBEGIN:
        ref = BT_Xbegin;
        break;
    case ZYDIS_MNEMONIC_XABORT:
        ref = BT_Xabort;
        break;
    case ZYDIS_MNEMONIC_RSM:
        ref = BT_Rsm;
        break;
    case ZYDIS_MNEMONIC_LOOP:
    case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
        ref = BT_Loop;
    default:
        ;
    }

    return (bt & ref) != 0;
}


ZydisMnemonic Zydis::GetId() const
{
    if(!Success())
        DebugBreak();
    return mZyInstr.mnemonic;
}

std::string Zydis::InstructionText(bool replaceRipRelative) const
{
    if(!Success())
        return "???";

    std::string result = mInstrText;
#ifdef _WIN64
    // TODO (ath): We can do that a whole lot sexier using formatter hooks
    if(replaceRipRelative)
    {
        //replace [rip +/- 0x?] with the actual address
        bool ripPlus = true;
        auto found = result.find("[rip + ");
        if(found == std::string::npos)
        {
            ripPlus = false;
            found = result.find("[rip - ");
        }
        if(found != std::string::npos)
        {
            auto wVA = Address();
            auto end = result.find("]", found);
            auto ripStr = result.substr(found + 1, end - found - 1);
            uint64_t offset;
            sscanf_s(ripStr.substr(ripStr.rfind(' ') + 1).c_str(), "%llX", &offset);
            auto dest = ripPlus ? (wVA + offset + Size()) : (wVA - offset + Size());
            char buf[20];
            sprintf_s(buf, "0x%llx", dest);
            result.replace(found + 1, ripStr.length(), buf);
        }
    }
#endif //_WIN64

    return result;
}

int Zydis::OpCount() const
{
    if(!Success())
        return 0;
    return mExplicitOpCount;
}

const ZydisDecodedOperand & Zydis::operator[](int index) const
{
    if(!Success() || index < 0 || index >= OpCount())
        DebugBreak();
    return mZyInstr.operands[index];
}

static bool isSafe64NopRegOp(const ZydisDecodedOperand & op)
{
    if(op.type != ZYDIS_OPERAND_TYPE_REGISTER)
        return true; //a non-register is safe
#ifdef _WIN64
    switch(op.reg.value)
    {
    case ZYDIS_REGISTER_EAX:
    case ZYDIS_REGISTER_EBX:
    case ZYDIS_REGISTER_ECX:
    case ZYDIS_REGISTER_EDX:
    case ZYDIS_REGISTER_EBP:
    case ZYDIS_REGISTER_ESP:
    case ZYDIS_REGISTER_ESI:
    case ZYDIS_REGISTER_EDI:
        return false; //32 bit register modifications clear the high part of the 64 bit register
    default:
        return true; //all other registers are safe
    }
#else
    return true;
#endif //_WIN64
}

bool Zydis::IsNop() const
{
    if(!Success())
        return false;

    const auto & ops = mZyInstr.operands;

    switch(mZyInstr.mnemonic)
    {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_FNOP:
        // nop
        return true;
    case ZYDIS_MNEMONIC_MOV:
	case ZYDIS_MNEMONIC_CMOVB:
	case ZYDIS_MNEMONIC_CMOVBE:
	case ZYDIS_MNEMONIC_CMOVL:
	case ZYDIS_MNEMONIC_CMOVLE:
	case ZYDIS_MNEMONIC_CMOVNB:
	case ZYDIS_MNEMONIC_CMOVNBE:
	case ZYDIS_MNEMONIC_CMOVNL:
	case ZYDIS_MNEMONIC_CMOVNLE:
	case ZYDIS_MNEMONIC_CMOVNO:
	case ZYDIS_MNEMONIC_CMOVNP:
	case ZYDIS_MNEMONIC_CMOVNS:
	case ZYDIS_MNEMONIC_CMOVNZ:
	case ZYDIS_MNEMONIC_CMOVO:
	case ZYDIS_MNEMONIC_CMOVP:
	case ZYDIS_MNEMONIC_CMOVS:
	case ZYDIS_MNEMONIC_CMOVZ:
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_XCHG:
        // mov edi, edi
		return ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER 
    		&& ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER 
    		&& ops[0].reg.value == ops[1].reg.value
    		&& isSafe64NopRegOp(ops[0]);
    case ZYDIS_MNEMONIC_LEA:
    {
        // lea eax, [eax + 0]
        auto reg = ops[0].reg.value;
        auto mem = ops[1].mem;
		return ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER 
    		&& ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER
    		&& mem.disp.value == 0 
    		&& ((mem.index == ZYDIS_REGISTER_NONE && mem.base == reg) ||
				(mem.index == reg && mem.base == ZYDIS_REGISTER_NONE && mem.scale == 1)) 
    		&& isSafe64NopRegOp(ops[0]);
    }
	case ZYDIS_MNEMONIC_JB:
	case ZYDIS_MNEMONIC_JBE:
	case ZYDIS_MNEMONIC_JCXZ:
	case ZYDIS_MNEMONIC_JECXZ:
	case ZYDIS_MNEMONIC_JKNZD:
	case ZYDIS_MNEMONIC_JKZD:
	case ZYDIS_MNEMONIC_JL:
	case ZYDIS_MNEMONIC_JLE:
	case ZYDIS_MNEMONIC_JMP:
	case ZYDIS_MNEMONIC_JNB:
	case ZYDIS_MNEMONIC_JNBE:
	case ZYDIS_MNEMONIC_JNL:
	case ZYDIS_MNEMONIC_JNLE:
	case ZYDIS_MNEMONIC_JNO:
	case ZYDIS_MNEMONIC_JNP:
	case ZYDIS_MNEMONIC_JNS:
	case ZYDIS_MNEMONIC_JNZ:
	case ZYDIS_MNEMONIC_JO:
	case ZYDIS_MNEMONIC_JP:
	case ZYDIS_MNEMONIC_JRCXZ:
	case ZYDIS_MNEMONIC_JS:
	case ZYDIS_MNEMONIC_JZ:
        // jmp 0
        return ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE 
    		&& ops[0].imm.value.u == this->Address() + this->Size();
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR:
    case ZYDIS_MNEMONIC_SAR:
        // shl eax, 0
		return ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
    		&& ops[1].imm.value.u == 0 
    		&& isSafe64NopRegOp(ops[0]);
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
        // shld eax, ebx, 0
		return ops[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
    		&& ops[2].imm.value.u == 0 
    		&& isSafe64NopRegOp(ops[0]) 
    		&& isSafe64NopRegOp(ops[1]);
    default:
        return false;
    }
}

bool Zydis::IsPushPop() const
{
    if(!Success())
        return false;

    switch(mZyInstr.meta.category)
    {
    case ZYDIS_CATEGORY_PUSH:
    case ZYDIS_CATEGORY_POP:
        return true;
    default:
        return false;
    }
}


bool Zydis::IsUnusual() const
{
    if(!Success())
        return false;

    auto id = mZyInstr.mnemonic;
    return mZyInstr.attributes & ZYDIS_ATTRIB_IS_PRIVILEGED
        || id == ZYDIS_MNEMONIC_RDTSC 
	    || id == ZYDIS_MNEMONIC_SYSCALL 
	    || id == ZYDIS_MNEMONIC_SYSENTER 
	    || id == ZYDIS_MNEMONIC_CPUID 
	    || id == ZYDIS_MNEMONIC_RDTSCP
        || id == ZYDIS_MNEMONIC_RDRAND 
	    || id == ZYDIS_MNEMONIC_RDSEED 
	    || id == ZYDIS_MNEMONIC_UD1 
	    || id == ZYDIS_MNEMONIC_UD2;
}

std::string Zydis::Mnemonic() const
{
    if(!Success())
        return "???";

	switch(mZyInstr.mnemonic)
	{
    case ZYDIS_MNEMONIC_JZ:
        return "je";
    case ZYDIS_MNEMONIC_JNZ:
        return "jne";
    case ZYDIS_MNEMONIC_JNBE:
        return "ja";
    case ZYDIS_MNEMONIC_JNB:
        return "jae";
    case ZYDIS_MNEMONIC_JNLE:
        return "jg";
    case ZYDIS_MNEMONIC_JNL:
        return "jge";
    case ZYDIS_MNEMONIC_CMOVNBE:
        return "cmova";
    case ZYDIS_MNEMONIC_CMOVNB:
        return "cmovae";
    case ZYDIS_MNEMONIC_CMOVZ:
        return "cmove";
    case ZYDIS_MNEMONIC_CMOVNLE:
        return "cmovg";
    case ZYDIS_MNEMONIC_CMOVNL:
        return "cmovge";
    case ZYDIS_MNEMONIC_CMOVNZ:
        return "cmovne";
    case ZYDIS_MNEMONIC_SETNBE:
        return "seta";
    case ZYDIS_MNEMONIC_SETNB:
        return "setae";
    case ZYDIS_MNEMONIC_SETZ:
        return "sete";
    case ZYDIS_MNEMONIC_SETNLE:
        return "setg";
    case ZYDIS_MNEMONIC_SETNL:
        return "setge";
    case ZYDIS_MNEMONIC_SETNZ:
        return "setne";
    default:
        return ZydisMnemonicGetString(mZyInstr.mnemonic);
	}
}

std::string Zydis::MnemonicId() const
{
    // hax
    return Mnemonic();
}

const char* Zydis::MemSizeName(int size) const
{
    switch(size)
    {
    case 1:
        return "byte";
    case 2:
        return "word";
    case 4:
        return "dword";
    case 6:
        return "fword";
    case 8:
        return "qword";
    case 10:
        return "tword";
    case 14:
        return "m14";
    case 16:
        return "xmmword";
    case 28:
        return "m28";
    case 32:
        return "yword";
    case 64:
        return "zword";
    default:
        return nullptr;
    }
}

size_t Zydis::BranchDestination() const
{
    if(!Success() 
        || mZyInstr.operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE
        || !mZyInstr.operands[0].imm.isRelative)
        return 0;

    return mZyInstr.operands[0].imm.value.u;
}

size_t Zydis::ResolveOpValue(int opindex, const std::function<size_t(ZydisRegister)> & resolveReg) const
{
    size_t dest = 0;
	const auto & op = mZyInstr.operands[opindex];
    switch(op.type)
    {
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        dest = size_t(op.imm.value.u);
        break;
    case ZYDIS_OPERAND_TYPE_REGISTER:
        dest = resolveReg(op.reg.value);
        break;
    case ZYDIS_OPERAND_TYPE_MEMORY:
        dest = size_t(op.mem.disp.value);
        if(op.mem.base == ZYDIS_REGISTER_RIP) //rip-relative
            dest += Address() + Size();
        else
            dest += resolveReg(op.mem.base) + resolveReg(op.mem.index) * op.mem.scale;
        break;
    default:
        break;
    }
    return dest;
}

bool Zydis::IsBranchGoingToExecute(size_t cflags, size_t ccx) const
{
    if(!Success())
        return false;
    return IsBranchGoingToExecute(mZyInstr.mnemonic, cflags, ccx);
}

bool Zydis::IsBranchGoingToExecute(ZydisMnemonic id, size_t cflags, size_t ccx)
{
    auto bCF = (cflags & (1 << 0)) != 0;
    auto bPF = (cflags & (1 << 2)) != 0;
    auto bZF = (cflags & (1 << 6)) != 0;
    auto bSF = (cflags & (1 << 7)) != 0;
    auto bOF = (cflags & (1 << 11)) != 0;
    switch(id)
    {
    case ZYDIS_MNEMONIC_CALL:
    case ZYDIS_MNEMONIC_JMP:
    case ZYDIS_MNEMONIC_RET:
        return true;
    case ZYDIS_MNEMONIC_JNB: //jump short if above or equal
        return !bCF;
	case ZYDIS_MNEMONIC_JNBE: //jump short if above
        return !bCF && !bZF;
    case ZYDIS_MNEMONIC_JBE: //jump short if below or equal/not above
        return bCF || bZF;
    case ZYDIS_MNEMONIC_JB: //jump short if below/not above nor equal/carry
        return bCF;
    case ZYDIS_MNEMONIC_JCXZ: //jump short if ecx register is zero
    case ZYDIS_MNEMONIC_JECXZ: //jump short if ecx register is zero
    case ZYDIS_MNEMONIC_JRCXZ: //jump short if rcx register is zero
        return ccx == 0;
    case ZYDIS_MNEMONIC_JZ: //jump short if equal
        return bZF;
    case ZYDIS_MNEMONIC_JNL: //jump short if greater or equal
        return bSF == bOF;
    case ZYDIS_MNEMONIC_JNLE: //jump short if greater
        return !bZF && bSF == bOF;
    case ZYDIS_MNEMONIC_JLE: //jump short if less or equal/not greater
        return bZF || bSF != bOF;
    case ZYDIS_MNEMONIC_JL: //jump short if less/not greater
        return bSF != bOF;
    case ZYDIS_MNEMONIC_JNZ: //jump short if not equal/not zero
        return !bZF;
    case ZYDIS_MNEMONIC_JNO: //jump short if not overflow
        return !bOF;
    case ZYDIS_MNEMONIC_JNP: //jump short if not parity/parity odd
        return !bPF;
    case ZYDIS_MNEMONIC_JNS: //jump short if not sign
        return !bSF;
    case ZYDIS_MNEMONIC_JO: //jump short if overflow
        return bOF;
    case ZYDIS_MNEMONIC_JP: //jump short if parity/parity even
        return bPF;
    case ZYDIS_MNEMONIC_JS: //jump short if sign
        return bSF;
    case ZYDIS_MNEMONIC_LOOP: //decrement count; jump short if ecx!=0
        return ccx != 0;
    case ZYDIS_MNEMONIC_LOOPE: //decrement count; jump short if ecx!=0 and zf=1
        return ccx != 0 && bZF;
    case ZYDIS_MNEMONIC_LOOPNE: //decrement count; jump short if ecx!=0 and zf=0
        return ccx != 0 && !bZF;
    default:
        return false;
    }
}

bool Zydis::IsConditionalGoingToExecute(size_t cflags, size_t ccx) const
{
    if(!Success())
        return false;
    return IsConditionalGoingToExecute(mZyInstr.mnemonic, cflags, ccx);
}

bool Zydis::IsConditionalGoingToExecute(ZydisMnemonic id, size_t cflags, size_t ccx)
{
    auto bCF = (cflags & (1 << 0)) != 0;
    auto bPF = (cflags & (1 << 2)) != 0;
    auto bZF = (cflags & (1 << 6)) != 0;
    auto bSF = (cflags & (1 << 7)) != 0;
    auto bOF = (cflags & (1 << 11)) != 0;
    switch(id)
    {
    case ZYDIS_MNEMONIC_CMOVNBE: //conditional move - above/not below nor equal
        return !bCF && !bZF;
    case ZYDIS_MNEMONIC_CMOVNB: //conditional move - above or equal/not below/not carry
        return !bCF;
    case ZYDIS_MNEMONIC_CMOVB: //conditional move - below/not above nor equal/carry
        return bCF;
    case ZYDIS_MNEMONIC_CMOVBE: //conditional move - below or equal/not above
        return bCF || bZF;
    case ZYDIS_MNEMONIC_CMOVZ: //conditional move - equal/zero
        return bZF;
    case ZYDIS_MNEMONIC_CMOVNLE: //conditional move - greater/not less nor equal
        return !bZF && bSF == bOF;
    case ZYDIS_MNEMONIC_CMOVNL: //conditional move - greater or equal/not less
        return bSF == bOF;
    case ZYDIS_MNEMONIC_CMOVL: //conditional move - less/not greater nor equal
        return bSF != bOF;
    case ZYDIS_MNEMONIC_CMOVLE: //conditional move - less or equal/not greater
        return bZF || bSF != bOF;
    case ZYDIS_MNEMONIC_CMOVNZ: //conditional move - not equal/not zero
        return !bZF;
    case ZYDIS_MNEMONIC_CMOVNO: //conditional move - not overflow
        return !bOF;
    case ZYDIS_MNEMONIC_CMOVNP: //conditional move - not parity/parity odd
        return !bPF;
    case ZYDIS_MNEMONIC_CMOVNS: //conditional move - not sign
        return !bSF;
    case ZYDIS_MNEMONIC_CMOVO: //conditional move - overflow
        return bOF;
    case ZYDIS_MNEMONIC_CMOVP: //conditional move - parity/parity even
        return bPF;
    case ZYDIS_MNEMONIC_CMOVS: //conditional move - sign
        return bSF;
    case ZYDIS_MNEMONIC_FCMOVBE: //fp conditional move - below or equal
        return bCF || bZF;
    case ZYDIS_MNEMONIC_FCMOVB: //fp conditional move - below
        return bCF;
    case ZYDIS_MNEMONIC_FCMOVE: //fp conditional move - equal
        return bZF;
    case ZYDIS_MNEMONIC_FCMOVNBE: //fp conditional move - not below or equal
        return !bCF && !bZF;
    case ZYDIS_MNEMONIC_FCMOVNB: //fp conditional move - not below
        return !bCF;
    case ZYDIS_MNEMONIC_FCMOVNE: //fp conditional move - not equal
        return !bZF;
    case ZYDIS_MNEMONIC_FCMOVNU: //fp conditional move - not unordered
        return !bPF;
    case ZYDIS_MNEMONIC_FCMOVU: //fp conditional move - unordered
        return bPF;
    case ZYDIS_MNEMONIC_SETNBE: //set byte on condition - above/not below nor equal
        return !bCF && !bZF;
    case ZYDIS_MNEMONIC_SETNB: //set byte on condition - above or equal/not below/not carry
        return !bCF;
    case ZYDIS_MNEMONIC_SETB: //set byte on condition - below/not above nor equal/carry
        return bCF;
    case ZYDIS_MNEMONIC_SETBE: //set byte on condition - below or equal/not above
        return bCF || bZF;
    case ZYDIS_MNEMONIC_SETZ: //set byte on condition - equal/zero
        return bZF;
    case ZYDIS_MNEMONIC_SETNLE: //set byte on condition - greater/not less nor equal
        return !bZF && bSF == bOF;
    case ZYDIS_MNEMONIC_SETNL: //set byte on condition - greater or equal/not less
        return bSF == bOF;
    case ZYDIS_MNEMONIC_SETL: //set byte on condition - less/not greater nor equal
        return bSF != bOF;
    case ZYDIS_MNEMONIC_SETLE: //set byte on condition - less or equal/not greater
        return bZF || bSF != bOF;
    case ZYDIS_MNEMONIC_SETNZ: //set byte on condition - not equal/not zero
        return !bZF;
    case ZYDIS_MNEMONIC_SETNO: //set byte on condition - not overflow
        return !bOF;
    case ZYDIS_MNEMONIC_SETNP: //set byte on condition - not parity/parity odd
        return !bPF;
    case ZYDIS_MNEMONIC_SETNS: //set byte on condition - not sign
        return !bSF;
    case ZYDIS_MNEMONIC_SETO: //set byte on condition - overflow
        return bOF;
    case ZYDIS_MNEMONIC_SETP: //set byte on condition - parity/parity even
        return bPF;
    case ZYDIS_MNEMONIC_SETS: //set byte on condition - sign
        return bSF;
    default:
        return true;
    }
}

void Zydis::RegInfo(uint8_t regs[ZYDIS_REGISTER_MAX_VALUE + 1]) const
{
    memset(regs, 0, sizeof(uint8_t) * (ZYDIS_REGISTER_MAX_VALUE + 1));
    if(!Success() || IsNop())
        return;
    for(int i = 0; i < OpCount(); i++)
    {
        const auto & op = mZyInstr.operands[i];
        switch(op.type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
        {
			if(op.action & ZYDIS_OPERAND_ACTION_MASK_READ)
				regs[op.reg.value] |= Read;
			if(op.action & ZYDIS_OPERAND_ACTION_MASK_WRITE)
				regs[op.reg.value] |= Write;
            regs[op.reg.value] |= op.visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT ?
                Explicit : Implicit;
        }
        break;

        case ZYDIS_OPERAND_TYPE_MEMORY:
        {
            if(op.mem.segment == ZYDIS_REGISTER_NONE)
            {
                switch(op.mem.base)
                {
#ifdef _WIN64
                case ZYDIS_REGISTER_RSP:
                case ZYDIS_REGISTER_RBP:
#else //x86
                case ZYDIS_REGISTER_ESP:
                case ZYDIS_REGISTER_EBP:
#endif //_WIN64
                    regs[ZYDIS_REGISTER_SS] |= Read | Explicit;
                    break;
                default:
                    regs[ZYDIS_REGISTER_DS] |= Read | Explicit;
                    break;
                }
            }
            else
                regs[op.mem.segment] |= Read | Explicit;
            regs[op.mem.base] |= Read | Explicit;
            regs[op.mem.index] |= Read | Explicit;
        }
        break;

        default:
            break;
        }
    }
}

const char* Zydis::FlagName(ZydisCPUFlag flag) const
{
    switch(flag)
    {
    case ZYDIS_CPUFLAG_AF:
        return "AF";
    case ZYDIS_CPUFLAG_CF:
        return "CF";
    case ZYDIS_CPUFLAG_SF:
        return "SF";
    case ZYDIS_CPUFLAG_ZF:
        return "ZF";
    case ZYDIS_CPUFLAG_PF:
        return "PF";
    case ZYDIS_CPUFLAG_OF:
        return "OF";
    case ZYDIS_CPUFLAG_TF:
        return "TF";
    case ZYDIS_CPUFLAG_IF:
        return "IF";
    case ZYDIS_CPUFLAG_DF:
        return "DF";
    case ZYDIS_CPUFLAG_NT:
        return "NT";
    case ZYDIS_CPUFLAG_RF:
        return "RF";
    default:
        return nullptr;
    }
}
