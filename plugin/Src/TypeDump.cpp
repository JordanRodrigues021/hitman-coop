#include "TypeDump.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "Globals.h"

#include <Glacier/Reflection.h>
#include <Glacier/ZTypeRegistry.h>

namespace {
std::string Lower(std::string p_Text) {
    std::transform(p_Text.begin(), p_Text.end(), p_Text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p_Text;
}

const char* NameOf(const STypeID* p_Id) {
    const IType* s_Type = p_Id ? p_Id->GetTypeInfo() : nullptr;
    return s_Type && s_Type->pszTypeName ? s_Type->pszTypeName : "?";
}
}

void TypeDump::Write(const std::string& p_Filter, const std::string& p_Path) {
    std::vector<std::string> s_Words;
    std::istringstream s_Stream(Lower(p_Filter));

    for (std::string s_Word; s_Stream >> s_Word;)
        s_Words.push_back(s_Word);

    std::map<std::string, const STypeID*> s_Matches;

    for (const auto& [s_Name, s_Id] : (*Globals::TypeRegistry)->m_types) {
        const std::string s_Lower = Lower(std::string(s_Name.c_str(), s_Name.size()));

        for (const auto& s_Word : s_Words) {
            if (s_Lower.find(s_Word) != std::string::npos) {
                s_Matches[std::string(s_Name.c_str(), s_Name.size())] = s_Id;
                break;
            }
        }
    }

    std::ofstream s_File(p_Path, std::ios::trunc);
    s_File << s_Matches.size() << " types matching '" << p_Filter << "'\n";

    for (const auto& [s_Name, s_Id] : s_Matches) {
        const IType* s_Type = s_Id ? s_Id->GetTypeInfo() : nullptr;

        if (s_Type && s_Type->IsEnum()) {
            const auto* s_Enum = reinterpret_cast<const IEnumType*>(s_Type);
            s_File << s_Name << " [enum]\n";

            for (const auto& s_Item : s_Enum->items)
                s_File << "    " << (s_Item.szName ? s_Item.szName : "?") << " = " << s_Item.nValue << "\n";

            continue;
        }

        if (!s_Type || !s_Type->IsClass()) {
            s_File << s_Name << "\n";
            continue;
        }

        const auto* s_Class = reinterpret_cast<const IClassType*>(s_Type);
        s_File << s_Name << (s_Type->IsEntity() ? " [entity]" : "") << " size " << s_Type->m_nTypeSize << "\n";

        for (uint16_t i = 0; i < s_Class->m_nBaseClasses; ++i)
            s_File << "    base " << NameOf(s_Class->m_pBaseClasses[i].m_Type) << "\n";

        for (uint16_t i = 0; i < s_Class->m_nInterfaces; ++i)
            s_File << "    interface " << NameOf(s_Class->m_pInterfaces[i].type) << "\n";

        for (uint16_t i = 0; i < s_Class->m_nProperties; ++i) {
            const auto& s_Property = s_Class->m_pProperties[i];
            s_File << "    property " << (s_Property.m_pszPropertyName ? s_Property.m_pszPropertyName : "?") << " : "
                   << NameOf(s_Property.m_propertyInfo.m_Type) << "  @+0x" << std::hex << s_Property.m_propertyInfo.m_nExtraData
                   << std::dec << "\n";
        }

        for (uint16_t i = 0; i < s_Class->m_nInputPins; ++i)
            s_File << "    input pin " << std::hex << s_Class->m_pInputPins[i].m_nPinID << std::dec << "\n";
    }
}
