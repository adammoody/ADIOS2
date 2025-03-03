/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BP5Serializer.h
 *
 */

#include "adios2/core/Attribute.h"
#include "adios2/core/Engine.h"
#include "adios2/core/IO.h"
#include "adios2/core/VariableBase.h"
#include "adios2/helper/adiosFunctions.h"
#include "adios2/helper/adiosType.h"

#include "BP5Deserializer.h"
#include "BP5Deserializer.tcc"

#ifdef ADIOS2_HAVE_ZFP
#include "adios2/operator/compress/CompressZFP.h"
#endif
#ifdef ADIOS2_HAVE_SZ
#include "adios2/operator/compress/CompressSZ.h"
#endif
#ifdef ADIOS2_HAVE_BZIP2
#include "adios2/operator/compress/CompressBZIP2.h"
#endif
#ifdef ADIOS2_HAVE_MGARD
#include "adios2/operator/compress/CompressMGARD.h"
#endif

#include "adios2/operator/OperatorFactory.h"

#include <array>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

using namespace adios2::helper;

#ifdef _WIN32
#pragma warning(disable : 4250)
#endif

namespace adios2
{
namespace format
{
static void ApplyElementMinMax(MinMaxStruct &MinMax, DataType Type,
                               void *Element);

void BP5Deserializer::InstallMetaMetaData(MetaMetaInfoBlock &MM)
{
    char *FormatID = (char *)malloc(MM.MetaMetaIDLen);
    char *MetaMetaInfo = (char *)malloc(MM.MetaMetaInfoLen);
    memcpy(FormatID, MM.MetaMetaID, MM.MetaMetaIDLen);
    memcpy(MetaMetaInfo, MM.MetaMetaInfo, MM.MetaMetaInfoLen);
    load_external_format_FMcontext(FMContext_from_FFS(ReaderFFSContext),
                                   FormatID, MM.MetaMetaIDLen, MetaMetaInfo);
    free(FormatID);
}

BP5Deserializer::ControlInfo *BP5Deserializer::GetPriorControl(FMFormat Format)
{
    struct ControlInfo *tmp = ControlBlocks;
    while (tmp)
    {
        if (tmp->Format == Format)
        {
            return tmp;
        }
        tmp = tmp->Next;
    }
    return NULL;
}

bool BP5Deserializer::NameIndicatesArray(const char *Name)
{
    return ((Name[2] == 'G') || (Name[2] == 'L') || (Name[2] == 'J'));
}

bool BP5Deserializer::NameIndicatesAttrArray(const char *Name)
{
    int Len = strlen(Name);
    return (strcmp("ElemCount", Name + Len - 9) == 0);
}

DataType BP5Deserializer::TranslateFFSType2ADIOS(const char *Type, int size)
{
    if (strcmp(Type, "integer") == 0)
    {
        if (size == 1)
        {
            return DataType::Int8;
        }
        else if (size == 2)
        {
            return DataType::Int16;
        }
        else if (size == 4)
        {
            return DataType::Int32;
        }
        else if (size == 8)
        {
            return DataType::Int64;
        }
    }
    else if (strcmp(Type, "unsigned integer") == 0)
    {
        if (size == 1)
        {
            return DataType::UInt8;
        }
        else if (size == 2)
        {
            return DataType::UInt16;
        }
        else if (size == 4)
        {
            return DataType::UInt32;
        }
        else if (size == 8)
        {
            return DataType::UInt64;
        }
    }
    else if ((strcmp(Type, "double") == 0) || (strcmp(Type, "float") == 0))
    {
        if (size == sizeof(float))
        {
            return DataType::Float;
        }
        else if ((sizeof(long double) != sizeof(double)) &&
                 (size == sizeof(long double)))
        {
            return DataType::LongDouble;
        }
        else
        {
            return DataType::Double;
        }
    }
    else if (strcmp(Type, "complex4") == 0)
    {
        return DataType::FloatComplex;
    }
    else if (strcmp(Type, "complex8") == 0)
    {
        return DataType::DoubleComplex;
    }
    else if (strcmp(Type, "string") == 0)
    {
        return DataType::String;
    }

    return DataType::None;
}

void BP5Deserializer::BreakdownVarName(const char *Name, char **base_name_p,
                                       DataType *type_p, int *element_size_p)
{
    int Type;
    int ElementSize;
    const char *NameStart = strchr(strchr(Name + 4, '_') + 1, '_') + 1;
    // + 4 to skip BP5_ or bp5_ prefix
    sscanf(Name + 4, "%d_%d_", &ElementSize, &Type);
    *element_size_p = ElementSize;
    *type_p = (DataType)Type;
    *base_name_p = strdup(NameStart);
}

void BP5Deserializer::BreakdownFieldType(const char *FieldType, bool &Operator,
                                         bool &MinMax)
{
    if (FieldType[0] != 'M')
    {
        throw std::runtime_error(
            "BP5 unable to parse metadata, likely old version");
    }

    // should start with "MetaArray"
    FieldType += strlen("MetaArray");
    if (FieldType == 0)
        return;
    if (FieldType[0] == 'O')
    {
        Operator = true;
        FieldType += strlen("Op");
    }
    if (FieldType[0] == 'M')
    {
        MinMax = true;
    }
}

void BP5Deserializer::BreakdownV1ArrayName(const char *Name, char **base_name_p,
                                           DataType *type_p,
                                           int *element_size_p, bool &Operator,
                                           bool &MinMax)
{
    int Type;
    int ElementSize;

    const char *NameStart = strchr(strchr(Name + 4, '_') + 1, '_') + 1;
    // + 3 to skip BP5_ or bp5_ prefix
    sscanf(Name + 4, "%d_%d", &ElementSize, &Type);
    const char *Plus = index(Name, '+');
    MinMax = false;
    while (Plus && (*Plus == '+'))
    {
        int Len;
        if (sscanf(Plus, "+%dO", &Len) == 1)
        { // Operator Spec
            Operator = true;
            const char *OpStart = index(Plus, 'O') + 1;
            Plus = OpStart + Len;
        }
        else if (strncmp(Plus, "+MM", 3) == 0)
        {
            MinMax = true;
            Plus += 3;
        }
    }
    *element_size_p = ElementSize;
    *type_p = (DataType)Type;
    *base_name_p = strdup(NameStart);
    *(rindex(*base_name_p, '_')) = 0;
}

void BP5Deserializer::BreakdownArrayName(const char *Name, char **base_name_p,
                                         DataType *type_p, int *element_size_p)
{
    int Type;
    int ElementSize;
    const char *NameStart = strchr(strchr(Name + 4, '_') + 1, '_') + 1;
    // + 3 to skip BP5_ or bp5_ prefix
    sscanf(Name + 4, "%d_%d", &ElementSize, &Type);
    *element_size_p = ElementSize;
    *type_p = (DataType)Type;
    *base_name_p = strdup(NameStart);
}

BP5Deserializer::BP5VarRec *BP5Deserializer::LookupVarByKey(void *Key) const
{
    auto ret = VarByKey.find(Key);
    return ret->second;
}

BP5Deserializer::BP5VarRec *BP5Deserializer::LookupVarByName(const char *Name)
{
    auto ret = VarByName[Name];
    return ret;
}

BP5Deserializer::BP5VarRec *BP5Deserializer::CreateVarRec(const char *ArrayName)
{
    BP5VarRec *Ret = new BP5VarRec();
    Ret->VarName = strdup(ArrayName);
    Ret->Variable = nullptr;
    Ret->VarNum = m_VarCount++;
    VarByName[Ret->VarName] = Ret;
    if (!m_RandomAccessMode)
    {
        const size_t writerCohortSize = WriterCohortSize(MaxSizeT);
        Ret->PerWriterMetaFieldOffset.resize(writerCohortSize);
        Ret->PerWriterBlockStart.resize(writerCohortSize);
    }
    return Ret;
}

BP5Deserializer::ControlInfo *BP5Deserializer::BuildControl(FMFormat Format)
{
    FMStructDescList FormatList = format_list_of_FMFormat(Format);
    FMFieldList FieldList = FormatList[0].field_list;
    while (strncmp(FieldList->field_name, "BitField", 8) == 0)
        FieldList++;
    while (FieldList->field_name &&
           (strncmp(FieldList->field_name, "DataBlockSize", 8) == 0))
        FieldList++;
    int i = 0;
    int ControlCount = 0;
    ControlInfo *ret = (BP5Deserializer::ControlInfo *)malloc(sizeof(*ret));
    ret->Format = Format;
    ret->MetaFieldOffset = new std::vector<size_t>();
    ret->CIVarIndex = new std::vector<size_t>();
    size_t VarIndex = 0;
    while (FieldList[i].field_name)
    {
        ret = (ControlInfo *)realloc(
            ret, sizeof(*ret) + ControlCount * sizeof(struct ControlInfo));
        struct ControlStruct *C = &(ret->Controls[ControlCount]);
        ControlCount++;

        C->FieldOffset = FieldList[i].field_offset;
        C->OrigShapeID = ShapeID::Unknown;
        switch (FieldList[i].field_name[2])
        {
        case 'g':
            C->OrigShapeID = ShapeID::GlobalValue;
            break;
        case 'G':
            C->OrigShapeID = ShapeID::GlobalArray;
            break;
        case 'J':
            C->OrigShapeID = ShapeID::JoinedArray;
            break;
        case 'l':
            C->OrigShapeID = ShapeID::LocalValue;
            break;
        case 'L':
            C->OrigShapeID = ShapeID::LocalArray;
            break;
        }
        BP5VarRec *VarRec = nullptr;
        if (NameIndicatesArray(FieldList[i].field_name))
        {
            char *ArrayName;
            DataType Type;
            int ElementSize;
            bool Operator = false;
            bool MinMax = false;
            bool V1_fields = true;
            if (FieldList[i].field_type[0] == 'M')
                V1_fields = false;
            if (V1_fields)
            {
                BreakdownV1ArrayName(FieldList[i + 4].field_name, &ArrayName,
                                     &Type, &ElementSize, Operator, MinMax);
            }
            else
            {
                BreakdownFieldType(FieldList[i].field_type, Operator, MinMax);
                BreakdownArrayName(FieldList[i].field_name, &ArrayName, &Type,
                                   &ElementSize);
            }
            VarRec = LookupVarByName(ArrayName);
            if (!VarRec)
            {
                VarRec = CreateVarRec(ArrayName);
                VarRec->Type = Type;
                VarRec->ElementSize = ElementSize;
                VarRec->OrigShapeID = C->OrigShapeID;
                if (Operator)
                    VarRec->Operator = strdup("SomeOperator");
                C->ElementSize = ElementSize;
            }
            C->VarRec = VarRec;
            size_t MetaRecFields = 7;
            if (Operator)
            {
                MetaRecFields++;
            }
            if (MinMax)
            {

                VarRec->MinMaxOffset = MetaRecFields * sizeof(void *);
                MetaRecFields++;
            }
            if (V1_fields)
            {
                i += MetaRecFields;
            }
            else
            {
                i++;
            }
            free(ArrayName);
        }
        else
        {
            /* simple field */
            char *FieldName = strdup(FieldList[i].field_name + 4); // skip BP5_
            VarRec = LookupVarByName(FieldName);
            if (!VarRec)
            {
                DataType Type = TranslateFFSType2ADIOS(FieldList[i].field_type,
                                                       FieldList[i].field_size);
                VarRec = CreateVarRec(FieldName);
                VarRec->DimCount = 0;
                C->Type = Type;
                VarRec->OrigShapeID = C->OrigShapeID;
                VarRec->Type = Type;
            }
            VarRec->ElementSize = FieldList[i].field_size;
            C->ElementSize = FieldList[i].field_size;
            C->VarRec = VarRec;
            free(FieldName);
            i++;
        }
        if (ret->MetaFieldOffset->size() <= VarRec->VarNum)
        {
            ret->MetaFieldOffset->resize(VarRec->VarNum + 1);
            ret->CIVarIndex->resize(VarRec->VarNum + 1);
        }
        (*ret->CIVarIndex)[VarRec->VarNum] = VarIndex;
        (*ret->MetaFieldOffset)[VarRec->VarNum] = C->FieldOffset;
        VarIndex++;
    }
    ret->ControlCount = ControlCount;
    ret->Next = ControlBlocks;
    ControlBlocks = ret;
    return ret;
}

void BP5Deserializer::ReverseDimensions(size_t *Dimensions, int count,
                                        int times)
{
    int Offset = 0;
    for (int j = 0; j < times; j++)
    {
        for (int i = 0; i < count / 2; i++)
        {
            size_t tmp = Dimensions[Offset + i];
            Dimensions[Offset + i] = Dimensions[Offset + count - i - 1];
            Dimensions[Offset + count - i - 1] = tmp;
        }
        Offset += count;
    }
}

void *BP5Deserializer::VarSetup(core::Engine *engine, const char *variableName,
                                const DataType Type, void *data)
{
    if (Type == adios2::DataType::Struct)
    {
        return (void *)NULL;
    }
#define declare_type(T)                                                        \
    else if (Type == helper::GetDataType<T>())                                 \
    {                                                                          \
        core::Variable<T> *variable =                                          \
            &(engine->m_IO.DefineVariable<T>(variableName));                   \
        variable->SetData((T *)data);                                          \
        variable->m_AvailableStepsCount = 1;                                   \
        return (void *)variable;                                               \
    }

    ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type

    return (void *)NULL;
};

void *BP5Deserializer::ArrayVarSetup(core::Engine *engine,
                                     const char *variableName,
                                     const DataType type, int DimCount,
                                     size_t *Shape, size_t *Start,
                                     size_t *Count)
{
    std::vector<size_t> VecShape;
    std::vector<size_t> VecStart;
    std::vector<size_t> VecCount;
    adios2::DataType Type = (adios2::DataType)type;
    /*
     * setup shape of array variable as global (I.E. Count == Shape,
     * Start == 0)
     */
    if (Shape)
    {
        for (int i = 0; i < DimCount; i++)
        {
            VecShape.push_back(Shape[i]);
            VecStart.push_back(0);
            VecCount.push_back(Shape[i]);
        }
    }
    else
    {
        VecShape = {};
        VecStart = {};
        for (int i = 0; i < DimCount; i++)
        {
            VecCount.push_back(Count[i]);
        }
    }

    if (Type == adios2::DataType::Struct)
    {
        return (void *)NULL;
    }
#define declare_type(T)                                                        \
    else if (Type == helper::GetDataType<T>())                                 \
    {                                                                          \
        core::Variable<T> *variable =                                          \
            &(engine->m_IO.DefineVariable<T>(variableName));                   \
        variable->m_Shape = VecShape;                                          \
        variable->m_Start = VecStart;                                          \
        variable->m_Count = VecCount;                                          \
        variable->m_AvailableStepsCount = 1;                                   \
        variable->m_ShapeID = ShapeID::GlobalArray;                            \
        variable->m_SingleValue = false;                                       \
        variable->m_Min = std::numeric_limits<T>::max();                       \
        variable->m_Max = std::numeric_limits<T>::min();                       \
        return (void *)variable;                                               \
    }
    ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type
    return (void *)NULL;
};

void BP5Deserializer::SetupForStep(size_t Step, size_t WriterCount)
{
    CurTimestep = Step;
    if (m_RandomAccessMode)
    {
        if (m_WriterCohortSize.size() < Step + 1)
        {
            m_WriterCohortSize.resize(Step + 1);
        }
        m_WriterCohortSize[Step] = WriterCount;
    }
    else
    {
        PendingRequests.clear();

        for (auto RecPair : VarByKey)
        {
            m_Engine->m_IO.RemoveVariable(RecPair.second->VarName);
            RecPair.second->Variable = NULL;
        }
        m_CurrentWriterCohortSize = WriterCount;
    }
}

size_t BP5Deserializer::WriterCohortSize(size_t Step) const
{
    if (m_RandomAccessMode)
    {
        if (Step < m_WriterCohortSize.size())
        {
            return m_WriterCohortSize[Step];
        }
        else
        {
            return m_WriterCohortSize.back();
        }
    }
    else
    {
        return m_CurrentWriterCohortSize;
    }
}

void BP5Deserializer::InstallMetaData(void *MetadataBlock, size_t BlockLen,
                                      size_t WriterRank, size_t Step)
{
    const size_t writerCohortSize = WriterCohortSize(Step);
    FFSTypeHandle FFSformat;
    void *BaseData;
    static int DumpMetadata = -1;
    FFSformat =
        FFSTypeHandle_from_encode(ReaderFFSContext, (char *)MetadataBlock);
    if (!FFSformat)
    {
        helper::Throw<std::logic_error>("Toolkit", "format::BP5Deserializer",
                                        "InstallMetaData",
                                        "Internal error or file corruption, no "
                                        "know format for Metadata Block");
    }
    if (!FFShas_conversion(FFSformat))
    {
        FMContext FMC = FMContext_from_FFS(ReaderFFSContext);
        FMFormat Format = FMformat_from_ID(FMC, (char *)MetadataBlock);
        FMStructDescList List =
            FMcopy_struct_list(format_list_of_FMFormat(Format));
        // GSE - restrict to homogenous FTM       FMlocalize_structs(List);
        establish_conversion(ReaderFFSContext, FFSformat, List);
        FMfree_struct_list(List);
    }
    if (FFSdecode_in_place_possible(FFSformat))
    {
        FFSdecode_in_place(ReaderFFSContext, (char *)MetadataBlock, &BaseData);
    }
    else
    {
        int DecodedLength = FFS_est_decode_length(
            ReaderFFSContext, (char *)MetadataBlock, BlockLen);
        BaseData = malloc(DecodedLength);
        FFSdecode_to_buffer(ReaderFFSContext, (char *)MetadataBlock, BaseData);
    }
    if (DumpMetadata == -1)
    {
        DumpMetadata = (getenv("BP5DumpMetadata") != NULL);
    }
    if (DumpMetadata)
    {
        printf("\nIncomingMetadatablock from WriterRank %d is %p :\n",
               (int)WriterRank, BaseData);
        FMdump_data(FMFormat_of_original(FFSformat), BaseData, 1024000);
        printf("\n\n");
    }
    struct ControlInfo *Control;
    struct ControlStruct *ControlFields;
    Control = GetPriorControl(FMFormat_of_original(FFSformat));
    if (!Control)
    {
        Control = BuildControl(FMFormat_of_original(FFSformat));
    }
    ControlFields = &Control->Controls[0];

    if (m_RandomAccessMode)
    {
        if (m_ControlArray.size() < Step + 1)
        {
            m_ControlArray.resize(Step + 1);
        }
        if (m_ControlArray[Step].size() == 0)
        {
            m_ControlArray[Step].resize(writerCohortSize);
        }
        m_ControlArray[Step][WriterRank] = Control;

        MetadataBaseArray.resize(Step + 1);
        if (MetadataBaseArray[Step] == nullptr)
        {
            m_MetadataBaseAddrs = new std::vector<void *>();
            m_MetadataBaseAddrs->resize(writerCohortSize);
            MetadataBaseArray[Step] = m_MetadataBaseAddrs;
            m_FreeableMBA = nullptr;
        }
    }
    else
    {
        if (!m_MetadataBaseAddrs)
        {
            m_MetadataBaseAddrs = new std::vector<void *>();
            m_FreeableMBA = m_MetadataBaseAddrs;
        }
        if (writerCohortSize > m_MetadataBaseAddrs->size())
        {
            m_MetadataBaseAddrs->resize(writerCohortSize);
        }
    }
    (*m_MetadataBaseAddrs)[WriterRank] = BaseData;

    for (int i = 0; i < Control->ControlCount; i++)
    {
        size_t FieldOffset = ControlFields[i].FieldOffset;
        BP5VarRec *VarRec = ControlFields[i].VarRec;
        void *field_data = (char *)BaseData + FieldOffset;
        if (!BP5BitfieldTest((BP5MetadataInfoStruct *)BaseData, i))
        {
            continue;
        }
        if (!m_RandomAccessMode)
        {
            if (writerCohortSize > VarRec->PerWriterBlockStart.size())
            {
                VarRec->PerWriterBlockStart.resize(writerCohortSize);
                VarRec->PerWriterMetaFieldOffset.resize(writerCohortSize);
            }
            VarRec->PerWriterMetaFieldOffset[WriterRank] = FieldOffset;
        }
        else
        {
            if ((VarRec->AbsStepFromRel.size() == 0) ||
                (VarRec->AbsStepFromRel.back() != Step))
            {
                VarRec->AbsStepFromRel.push_back(Step);
            }
        }
        if ((ControlFields[i].OrigShapeID == ShapeID::GlobalArray) ||
            (ControlFields[i].OrigShapeID == ShapeID::LocalArray))
        {
            MetaArrayRec *meta_base = (MetaArrayRec *)field_data;
            size_t BlockCount =
                meta_base->Dims ? meta_base->DBCount / meta_base->Dims : 1;
            if ((meta_base->Dims > 1) &&
                (m_WriterIsRowMajor != m_ReaderIsRowMajor))
            {
                /* if we're getting data from someone of the other array gender,
                 * switcheroo */
                ReverseDimensions(meta_base->Shape, meta_base->Dims, 1);
                ReverseDimensions(meta_base->Count, meta_base->Dims,
                                  BlockCount);
                ReverseDimensions(meta_base->Offsets, meta_base->Dims,
                                  BlockCount);
            }
            if ((WriterRank == 0) || (VarRec->GlobalDims == NULL))
            {
                // use the shape from rank 0 (or first non-NULL)
                VarRec->GlobalDims = meta_base->Shape;
            }
            if (!VarRec->Variable)
            {
                VarRec->Variable = ArrayVarSetup(
                    m_Engine, VarRec->VarName, VarRec->Type, meta_base->Dims,
                    meta_base->Shape, meta_base->Offsets, meta_base->Count);
                static_cast<VariableBase *>(VarRec->Variable)->m_Engine =
                    m_Engine;
                VarByKey[VarRec->Variable] = VarRec;
                VarRec->LastTSAdded = Step; // starts at 1
                if (!meta_base->Shape)
                {
                    static_cast<VariableBase *>(VarRec->Variable)->m_ShapeID =
                        ShapeID::LocalArray;
                }
            }

            VarRec->DimCount = meta_base->Dims;
            if (!m_RandomAccessMode)
            {
                if (WriterRank == 0)
                {
                    VarRec->PerWriterBlockStart[WriterRank] = 0;
                    if (writerCohortSize > 1)
                        VarRec->PerWriterBlockStart[WriterRank + 1] =
                            BlockCount;
                }
                if (WriterRank < static_cast<size_t>(writerCohortSize - 1))
                {
                    VarRec->PerWriterBlockStart[WriterRank + 1] =
                        VarRec->PerWriterBlockStart[WriterRank] + BlockCount;
                }
            }
        }
        else
        {
            if (!VarRec->Variable)
            {
                if (ControlFields[i].OrigShapeID == ShapeID::LocalValue)
                {
                    // Local single values show up as global arrays on the
                    // reader
                    size_t zero = 0;
                    size_t writerSize = writerCohortSize;
                    VarRec->Variable =
                        ArrayVarSetup(m_Engine, VarRec->VarName, VarRec->Type,
                                      1, &writerSize, &zero, &writerSize);
                    auto VB = static_cast<VariableBase *>(VarRec->Variable);
                    static_cast<VariableBase *>(VarRec->Variable)->m_Engine =
                        m_Engine;
                    VB->m_ShapeID = ShapeID::GlobalArray;
                }
                else
                {
                    // Global single value
                    VarRec->Variable = VarSetup(m_Engine, VarRec->VarName,
                                                VarRec->Type, field_data);
                    static_cast<VariableBase *>(VarRec->Variable)->m_Engine =
                        m_Engine;
                }
                VarByKey[VarRec->Variable] = VarRec;
                VarRec->LastTSAdded = Step;
            }
        }
        if (VarRec->FirstTSSeen == SIZE_MAX)
        {
            VarRec->FirstTSSeen = Step;
        }
        if (m_RandomAccessMode && (VarRec->LastTSAdded != Step))
        {
            static_cast<VariableBase *>(VarRec->Variable)
                ->m_AvailableStepsCount++;
            VarRec->LastTSAdded = Step;
        }
    }
}

void BP5Deserializer::InstallAttributeData(void *AttributeBlock,
                                           size_t BlockLen, size_t Step)
{
    static int DumpMetadata = -1;
    FMFieldList FieldList;
    FMStructDescList FormatList;
    void *BaseData;
    FFSTypeHandle FFSformat;

    if (BlockLen == 0)
        return;

    if (Step != m_LastAttrStep)
    {
        m_Engine->m_IO.RemoveAllAttributes();
        m_LastAttrStep = Step;
    }
    FFSformat =
        FFSTypeHandle_from_encode(ReaderFFSContext, (char *)AttributeBlock);
    if (!FFSformat)
    {
        helper::Throw<std::logic_error>(
            "Toolkit", "format::BP5Deserializer", "InstallAttributeData",
            "Internal error or file corruption, no know "
            "format for Attribute Block");
    }
    if (!FFShas_conversion(FFSformat))
    {
        FMContext FMC = FMContext_from_FFS(ReaderFFSContext);
        FMFormat Format = FMformat_from_ID(FMC, (char *)AttributeBlock);
        FMStructDescList List =
            FMcopy_struct_list(format_list_of_FMFormat(Format));
        // GSE - restrict to homogenous FTM       FMlocalize_structs(List);
        establish_conversion(ReaderFFSContext, FFSformat, List);
        FMfree_struct_list(List);
    }

    if (FFSdecode_in_place_possible(FFSformat))
    {
        FFSdecode_in_place(ReaderFFSContext, (char *)AttributeBlock, &BaseData);
    }
    else
    {
        int DecodedLength = FFS_est_decode_length(
            ReaderFFSContext, (char *)AttributeBlock, BlockLen);
        BaseData = malloc(DecodedLength);
        FFSBuffer decode_buf =
            create_fixed_FFSBuffer((char *)BaseData, DecodedLength);
        FFSdecode_to_buffer(ReaderFFSContext, (char *)AttributeBlock,
                            decode_buf);
    }
    if (DumpMetadata == -1)
    {
        DumpMetadata = (getenv("BP5DumpMetadata") != NULL);
    }
    if (DumpMetadata)
    {
        printf("\nIncomingAttributeDatablock (Step %zu) is %p :\n", Step,
               BaseData);
        FMdump_data(FMFormat_of_original(FFSformat), BaseData, 1024000);
        printf("\n\n");
    }
    FormatList = format_list_of_FMFormat(FMFormat_of_original(FFSformat));
    FieldList = FormatList[0].field_list;
    int i = 0;
    while (FieldList[i].field_name)
    {
        char *FieldName;
        void *field_data = (char *)BaseData + FieldList[i].field_offset;

        if (!NameIndicatesAttrArray(FieldList[i].field_name))
        {
            DataType Type;
            int ElemSize;
            BreakdownVarName(FieldList[i].field_name, &FieldName, &Type,
                             &ElemSize);
            if (Type == adios2::DataType::Struct)
            {
                return;
            }
            else if (Type == helper::GetDataType<std::string>())
            {
                m_Engine->m_IO.DefineAttribute<std::string>(
                    FieldName, *(char **)field_data, "", "/", true);
            }
#define declare_type(T)                                                        \
    else if (Type == helper::GetDataType<T>())                                 \
    {                                                                          \
        m_Engine->m_IO.DefineAttribute<T>(FieldName, *(T *)field_data, "",     \
                                          "/", true);                          \
    }

            ADIOS2_FOREACH_PRIMITIVE_STDTYPE_1ARG(declare_type)
#undef declare_type
            else
            {
                std::cout << "Loading attribute matched no type "
                          << ToString(Type) << std::endl;
            }
            free(FieldName);
            i++;
        }
        else
        {
            DataType Type;
            size_t ElemCount = *(size_t *)field_data;
            field_data = (void *)((char *)field_data + sizeof(size_t));
            i++;
            char *FieldName = strdup(FieldList[i].field_name + 4); // skip BP5_
            char *FieldType = strdup(FieldList[i].field_type);
            *index(FieldType, '[') = 0;
            Type = (DataType)TranslateFFSType2ADIOS(FieldType,
                                                    FieldList[i].field_size);
            if (Type == adios2::DataType::Struct)
            {
                return;
            }
            else if (Type == helper::GetDataType<std::string>())
            {
                std::vector<std::string> array;
                array.resize(ElemCount);
                char **str_array = *(char ***)field_data;
                for (size_t i = 0; i < ElemCount; i++)
                {
                    array[i].assign(str_array[i]);
                }
                m_Engine->m_IO.DefineAttribute<std::string>(
                    FieldName, array.data(), array.size(), "", "/", true);
            }
#define declare_type(T)                                                        \
    else if (Type == helper::GetDataType<T>())                                 \
    {                                                                          \
        T **array = *(T ***)field_data;                                        \
        m_Engine->m_IO.DefineAttribute<T>(FieldName, (T *)array, ElemCount,    \
                                          "", "/", true);                      \
    }

            ADIOS2_FOREACH_PRIMITIVE_STDTYPE_1ARG(declare_type)
#undef declare_type
            else
            {
                std::cout << "Loading attribute matched no type "
                          << ToString(Type) << std::endl;
            }
            free(FieldName);
            i++;
        }
    }
}

bool BP5Deserializer::QueueGet(core::VariableBase &variable, void *DestData)
{
    if (!m_RandomAccessMode)
    {
        return QueueGetSingle(variable, DestData, CurTimestep);
    }
    else
    {
        BP5VarRec *VarRec = VarByKey[&variable];
        bool ret = false;
        if (variable.m_StepsStart + variable.m_StepsCount >
            VarRec->AbsStepFromRel.size())
        {
            helper::Throw<std::invalid_argument>(
                "Toolkit", "format::BP5Deserializer", "QueueGet",
                "offset " + std::to_string(variable.m_StepsCount) +
                    " from steps start " +
                    std::to_string(variable.m_StepsStart) + " in variable " +
                    variable.m_Name +
                    " is beyond the largest available relative step = " +
                    std::to_string(VarRec->AbsStepFromRel.size()) +
                    ", check Variable SetStepSelection argument stepsCount "
                    "(random access), or "
                    "number of BeginStep calls (streaming)");
        }
        for (size_t RelStep = variable.m_StepsStart;
             RelStep < variable.m_StepsStart + variable.m_StepsCount; RelStep++)
        {
            const size_t AbsStep = VarRec->AbsStepFromRel[RelStep];
            const size_t writerCohortSize = WriterCohortSize(AbsStep);
            for (size_t WriterRank = 0; WriterRank < writerCohortSize;
                 WriterRank++)
            {
                if (GetMetadataBase(VarRec, AbsStep, WriterRank))
                {
                    // This writer wrote on this timestep
                    ret = QueueGetSingle(variable, DestData, AbsStep);
                    size_t increment =
                        variable.TotalSize() * variable.m_ElementSize;
                    DestData = (void *)((char *)DestData + increment);
                    break;
                }
            }
        }
        return ret;
    }
}

bool BP5Deserializer::GetSingleValueFromMetadata(core::VariableBase &variable,
                                                 BP5VarRec *VarRec,
                                                 void *DestData, size_t Step,
                                                 size_t WriterRank)
{
    char *src = (char *)GetMetadataBase(VarRec, Step, WriterRank);

    if (!src)
        return false;

    if (variable.m_SelectionType == adios2::SelectionType::WriteBlock)
        WriterRank = variable.m_BlockID;

    if (variable.m_Type != DataType::String)
    {
        memcpy(DestData, src, variable.m_ElementSize);
    }
    else
    {
        std::string *TmpStr = static_cast<std::string *>(DestData);
        TmpStr->assign(*(const char **)src);
    }
    return true;
}

bool BP5Deserializer::QueueGetSingle(core::VariableBase &variable,
                                     void *DestData, size_t Step)
{
    BP5VarRec *VarRec = VarByKey[&variable];
    if (VarRec->OrigShapeID == ShapeID::GlobalValue)
    {
        const size_t writerCohortSize = WriterCohortSize(Step);
        for (size_t WriterRank = 0; WriterRank < writerCohortSize; WriterRank++)
        {
            if (GetSingleValueFromMetadata(variable, VarRec, DestData, Step,
                                           WriterRank))
                return false;
        }
        return false;
    }
    if (VarRec->OrigShapeID == ShapeID::LocalValue)
    {
        // Shows up as global array with one element per writer rank
        for (size_t WriterRank = variable.m_Start[0];
             WriterRank < variable.m_Count[0] + variable.m_Start[0];
             WriterRank++)
        {
            (void)GetSingleValueFromMetadata(variable, VarRec, DestData, Step,
                                             WriterRank);
            DestData = (char *)DestData +
                       variable.m_ElementSize; // use variable.m_ElementSize
                                               // because it's the size in local
                                               // memory, VarRec->ElementSize is
                                               // the size in metadata
        }
        return false;
    }
    MemorySpace MemSpace = MemorySpace::Host;
    if (variable.IsCUDAPointer(DestData))
        MemSpace = MemorySpace::CUDA;
    if ((variable.m_SelectionType == adios2::SelectionType::BoundingBox) &&
        (variable.m_ShapeID == ShapeID::GlobalArray))
    {
        BP5ArrayRequest Req;
        Req.VarRec = VarRec;
        Req.RequestType = Global;
        Req.BlockID = variable.m_BlockID;
        Req.Count = variable.m_Count;
        Req.Start = variable.m_Start;
        Req.Step = Step;
        Req.MemSpace = MemSpace;
        Req.Data = DestData;
        PendingRequests.push_back(Req);
    }
    else if ((variable.m_SelectionType == adios2::SelectionType::WriteBlock) ||
             (variable.m_ShapeID == ShapeID::LocalArray))
    {
        BP5ArrayRequest Req;
        Req.VarRec = VarByKey[&variable];
        Req.RequestType = Local;
        Req.BlockID = variable.m_BlockID;
        if (variable.m_SelectionType == adios2::SelectionType::BoundingBox)
        {
            Req.Start = variable.m_Start;
            Req.Count = variable.m_Count;
        }
        Req.Data = DestData;
        Req.Step = Step;
        PendingRequests.push_back(Req);
    }
    else
    {
        std::cout << "Missed get type " << variable.m_SelectionType << " shape "
                  << variable.m_ShapeID << std::endl;
    }
    return true;
}

static bool IntersectionStartCount(const size_t dimensionsSize,
                                   const size_t *start1, const size_t *count1,
                                   const size_t *start2, const size_t *count2,
                                   size_t *outstart, size_t *outcount) noexcept
{
    for (size_t d = 0; d < dimensionsSize; ++d)
    {
        // Don't intercept
        const size_t end1 = start1[d] + count1[d] - 1;
        const size_t end2 = start2[d] + count2[d] - 1;

        if ((count1[d] == 0) || (count2[d] == 0))
        {
            return false;
        }
        if (start2[d] > end1 || end2 < start1[d])
        {
            return false;
        }
    }
    for (size_t d = 0; d < dimensionsSize; d++)
    {
        const size_t intersectionStart =
            (start1[d] < start2[d]) ? start2[d] : start1[d];

        // end, must be inclusive
        const size_t end1 = start1[d] + count1[d] - 1;
        const size_t end2 = start2[d] + count2[d] - 1;
        const size_t intersectionEnd = (end1 > end2) ? end2 : end1;
        outstart[d] = intersectionStart;
        outcount[d] = intersectionEnd - intersectionStart + 1;
        if (outcount[d] == 0)
            return false;
    }
    return true;
}

static size_t LinearIndex(const size_t dimensionsSize, const size_t *count,
                          const size_t *pos, bool IsRowMajor) noexcept
{
    size_t offset = 0;
    if (IsRowMajor)
    {
        for (size_t d = 0; d < dimensionsSize; ++d)
        {
            offset = offset * count[d] + pos[d];
        }
    }
    else
    {
        for (size_t d = dimensionsSize - 1; d < dimensionsSize; d--)
        {
            offset = offset * count[d] + pos[d];
        }
    }
    return offset;
}

static size_t CalcBlockLength(const size_t dimensionsSize, const size_t *count)
{
    size_t len = count[0];
    for (size_t d = 1; d < dimensionsSize; ++d)
    {
        len = len * count[d];
    }
    return len;
}

std::vector<BP5Deserializer::ReadRequest>
BP5Deserializer::GenerateReadRequests(const bool doAllocTempBuffers,
                                      size_t *maxReadSize)
{
    std::vector<BP5Deserializer::ReadRequest> Ret;
    *maxReadSize = 0;

    for (size_t ReqIndex = 0; ReqIndex < PendingRequests.size(); ReqIndex++)
    {
        auto Req = &PendingRequests[ReqIndex];
        if (Req->RequestType == Local)
        {
            const size_t writerCohortSize = WriterCohortSize(Req->Step);
            size_t NodeFirstBlock = 0;
            for (size_t WriterRank = 0; WriterRank < writerCohortSize;
                 WriterRank++)
            {
                MetaArrayRecOperator *writer_meta_base =
                    (MetaArrayRecOperator *)GetMetadataBase(
                        Req->VarRec, Req->Step, WriterRank);
                if (!writer_meta_base)
                {
                    continue; // Not writen on this step
                }
                size_t NodeLastBlock =
                    NodeFirstBlock + writer_meta_base->BlockCount - 1;
                if ((NodeFirstBlock <= Req->BlockID) &&
                    (NodeLastBlock >= Req->BlockID))
                {
                    // block is here
                    size_t NeededBlock = Req->BlockID - NodeFirstBlock;
                    size_t StartDim = NeededBlock * Req->VarRec->DimCount;
                    ReadRequest RR;
                    RR.Timestep = Req->Step;
                    RR.WriterRank = WriterRank;
                    RR.StartOffset =
                        writer_meta_base->DataBlockLocation[NeededBlock];

                    RR.ReadLength =
                        helper::GetDataTypeSize(Req->VarRec->Type) *
                        CalcBlockLength(Req->VarRec->DimCount,
                                        &writer_meta_base->Count[StartDim]);
                    RR.DestinationAddr = nullptr;
                    if (doAllocTempBuffers)
                    {
                        RR.DestinationAddr = (char *)malloc(RR.ReadLength);
                    }
                    *maxReadSize =
                        (*maxReadSize < RR.ReadLength ? RR.ReadLength
                                                      : *maxReadSize);
                    RR.Internal = NULL;
                    RR.OffsetInBlock = 0;
                    RR.ReqIndex = ReqIndex;
                    RR.BlockID = NeededBlock;
                    Ret.push_back(RR);
                    break;
                }
                NodeFirstBlock += writer_meta_base->BlockCount;
            }
        }
        else
        {
            /* global case */
            const size_t writerCohortSize = WriterCohortSize(Req->Step);
            for (size_t WriterRank = 0; WriterRank < writerCohortSize;
                 WriterRank++)
            {
                MetaArrayRecOperator *writer_meta_base =
                    (MetaArrayRecOperator *)GetMetadataBase(
                        Req->VarRec, Req->Step, WriterRank);
                if (!writer_meta_base)
                    continue; // Not writen on this step

                for (size_t Block = 0; Block < writer_meta_base->BlockCount;
                     Block++)
                {
                    std::array<size_t, helper::MAX_DIMS> intersectionstart;
                    std::array<size_t, helper::MAX_DIMS> intersectioncount;

                    size_t StartDim = Block * Req->VarRec->DimCount;
                    if (IntersectionStartCount(
                            Req->VarRec->DimCount, Req->Start.data(),
                            Req->Count.data(),
                            &writer_meta_base->Offsets[StartDim],
                            &writer_meta_base->Count[StartDim],
                            &intersectionstart[0], &intersectioncount[0]))
                    {
                        if (Req->VarRec->Operator != NULL)
                        {
                            // need the whole thing for decompression anyway
                            ReadRequest RR;
                            RR.Timestep = Req->Step;
                            RR.WriterRank = WriterRank;
                            RR.StartOffset =
                                writer_meta_base->DataBlockLocation[Block];
                            RR.ReadLength =
                                writer_meta_base->DataBlockSize[Block];
                            RR.DestinationAddr = nullptr;
                            if (doAllocTempBuffers)
                            {
                                RR.DestinationAddr =
                                    (char *)malloc(RR.ReadLength);
                            }
                            *maxReadSize =
                                (*maxReadSize < RR.ReadLength ? RR.ReadLength
                                                              : *maxReadSize);
                            RR.Internal = NULL;
                            RR.ReqIndex = ReqIndex;
                            RR.BlockID = Block;
                            RR.OffsetInBlock = 0;
                            Ret.push_back(RR);
                        }
                        else
                        {
                            for (size_t Dim = 0; Dim < Req->VarRec->DimCount;
                                 Dim++)
                            {
                                intersectionstart[Dim] -=
                                    writer_meta_base->Offsets[StartDim + Dim];
                            }
                            size_t StartOffsetInBlock =
                                helper::GetDataTypeSize(Req->VarRec->Type) *
                                LinearIndex(Req->VarRec->DimCount,
                                            &writer_meta_base->Count[StartDim],
                                            &intersectionstart[0],
                                            m_ReaderIsRowMajor);
                            for (size_t Dim = 0; Dim < Req->VarRec->DimCount;
                                 Dim++)
                            {
                                intersectionstart[Dim] +=
                                    intersectioncount[Dim] - 1;
                            }
                            size_t EndOffsetInBlock =
                                helper::GetDataTypeSize(Req->VarRec->Type) *
                                (LinearIndex(Req->VarRec->DimCount,
                                             &writer_meta_base->Count[StartDim],
                                             &intersectionstart[0],
                                             m_ReaderIsRowMajor) +
                                 1);
                            ReadRequest RR;
                            RR.Timestep = Req->Step;
                            RR.WriterRank = WriterRank;
                            RR.StartOffset =
                                writer_meta_base->DataBlockLocation[Block] +
                                StartOffsetInBlock;
                            RR.ReadLength =
                                EndOffsetInBlock - StartOffsetInBlock;
                            RR.DestinationAddr = nullptr;
                            if (doAllocTempBuffers)
                            {
                                RR.DestinationAddr =
                                    (char *)malloc(RR.ReadLength);
                            }
                            *maxReadSize =
                                (*maxReadSize < RR.ReadLength ? RR.ReadLength
                                                              : *maxReadSize);
                            RR.Internal = NULL;
                            RR.OffsetInBlock = StartOffsetInBlock;
                            RR.ReqIndex = ReqIndex;
                            RR.BlockID = Block;
                            Ret.push_back(RR);
                        }
                    }
                }
            }
        }
    }
    return Ret;
}

void BP5Deserializer::FinalizeGet(const ReadRequest &Read, const bool freeAddr)
{
    auto Req = PendingRequests[Read.ReqIndex];
    /*std::cout << "    Req: block = " << Req.BlockID << " step = " << Req.Step
              << " var = " << Req.VarRec->VarName << " start = " << Req.Start
              << " count = " << Req.Count << " dest "
              << reinterpret_cast<size_t>(Req.Data) << std::endl;*/
    int ElementSize = Req.VarRec->ElementSize;
    MetaArrayRec *writer_meta_base =
        (MetaArrayRec *)GetMetadataBase(Req.VarRec, Req.Step, Read.WriterRank);

    size_t *GlobalDimensions = writer_meta_base->Shape;
    int DimCount = writer_meta_base->Dims;
    std::vector<size_t> ZeroSel(DimCount);
    size_t *RankOffset = &writer_meta_base->Offsets[DimCount * Read.BlockID];
    size_t *RankSize = &writer_meta_base->Count[DimCount * Read.BlockID];
    std::vector<size_t> ZeroRankOffset(DimCount);
    std::vector<size_t> ZeroGlobalDimensions(DimCount);
    const size_t *SelOffset = NULL;
    const size_t *SelSize = NULL;
    char *IncomingData = Read.DestinationAddr;
    char *VirtualIncomingData = Read.DestinationAddr - Read.OffsetInBlock;
    std::vector<char> decompressBuffer;
    if (Req.VarRec->Operator != NULL)
    {
        size_t DestSize = Req.VarRec->ElementSize;
        for (size_t dim = 0; dim < Req.VarRec->DimCount; dim++)
        {
            DestSize *=
                writer_meta_base
                    ->Count[dim + Read.BlockID * writer_meta_base->Dims];
        }
        decompressBuffer.resize(DestSize);
        {
            std::lock_guard<std::mutex> lockGuard(mutexDecompress);
            core::Decompress(IncomingData,
                             ((MetaArrayRecOperator *)writer_meta_base)
                                 ->DataBlockSize[Read.BlockID],
                             decompressBuffer.data());
        }
        IncomingData = decompressBuffer.data();
        VirtualIncomingData = IncomingData;
    }
    if (Req.Start.size())
    {
        SelOffset = Req.Start.data();
    }
    if (Req.Count.size())
    {
        SelSize = Req.Count.data();
    }
    if (Req.RequestType == Local)
    {
        RankOffset = ZeroRankOffset.data();
        GlobalDimensions = ZeroGlobalDimensions.data();
        if (SelSize == NULL)
        {
            SelSize = RankSize;
        }
        if (SelOffset == NULL)
        {
            SelOffset = ZeroSel.data();
        }
        for (int i = 0; i < DimCount; i++)
        {
            GlobalDimensions[i] = RankSize[i];
        }
    }

    DimsArray inStart(DimCount, RankOffset);
    DimsArray inCount(DimCount, RankSize);
    DimsArray outStart(DimCount, SelOffset);
    DimsArray outCount(DimCount, SelSize);
    if (!m_ReaderIsRowMajor)
    {
        std::reverse(inStart.begin(), inStart.end());
        std::reverse(inCount.begin(), inCount.end());
        std::reverse(outStart.begin(), outStart.end());
        std::reverse(outCount.begin(), outCount.end());
    }

    helper::NdCopy(VirtualIncomingData, inStart, inCount, true, true,
                   (char *)Req.Data, outStart, outCount, true, true,
                   ElementSize, CoreDims(), CoreDims(), CoreDims(), CoreDims(),
                   false, Req.MemSpace);
    if (freeAddr)
    {
        free((char *)Read.DestinationAddr);
    }
}

void BP5Deserializer::FinalizeGets(std::vector<ReadRequest> &Reads)
{
    for (const auto &Read : Reads)
    {
        FinalizeGet(Read, true);
    }
    PendingRequests.clear();
}

void BP5Deserializer::MapGlobalToLocalIndex(size_t Dims,
                                            const size_t *GlobalIndex,
                                            const size_t *LocalOffsets,
                                            size_t *LocalIndex)
{
    for (size_t i = 0; i < Dims; i++)
    {
        LocalIndex[i] = GlobalIndex[i] - LocalOffsets[i];
    }
}

int BP5Deserializer::FindOffset(size_t Dims, const size_t *Size,
                                const size_t *Index)
{
    int Offset = 0;
    for (size_t i = 0; i < Dims; i++)
    {
        Offset = Index[i] + (Size[i] * Offset);
    }
    return Offset;
}

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/*
 *  - ElementSize is the byte size of the array elements
 *  - Dims is the number of dimensions in the variable
 *  - GlobalDims is an array, Dims long, giving the size of each dimension
 *  - PartialOffsets is an array, Dims long, giving the starting offsets per
 *    dimension of this data block in the global array
 *  - PartialCounts is an array, Dims long, giving the size per dimension
 *    of this data block in the global array
 *  - SelectionOffsets is an array, Dims long, giving the starting offsets in
 * the
 *    global array of the output selection.
 *  - SelectionCounts is an array, Dims long, giving the size per dimension
 *    of the output selection.
 *  - InData is the input, a slab of the global array
 *  - OutData is the output, to be filled with the selection array.
 */

/*
 * *******************************
 *
 * ExtractSelectionFromPartial*M both need to be extended to work when
 * the reader and writer have different byte orders.  This involves at
 * least supporting simple big/little-endian byte reversal, but a true
 * archival format should also consider mixed and middle-endian
 * hybrids.  This would require changes to the BP5 header so that the
 * appropriate transformations could be determined.
 *
 * *******************************
 */

BP5Deserializer::BP5Deserializer(bool WriterIsRowMajor, bool ReaderIsRowMajor,
                                 bool RandomAccessMode)
: m_WriterIsRowMajor{WriterIsRowMajor}, m_ReaderIsRowMajor{ReaderIsRowMajor},
  m_RandomAccessMode{RandomAccessMode}
{
    FMContext Tmp = create_local_FMcontext();
    ReaderFFSContext = create_FFSContext_FM(Tmp);
    free_FMcontext(Tmp);
}

BP5Deserializer::~BP5Deserializer()
{
    struct ControlInfo *tmp = ControlBlocks;
    free_FFSContext(ReaderFFSContext);
    ControlBlocks = NULL;
    while (tmp)
    {
        struct ControlInfo *next = tmp->Next;
        delete tmp->MetaFieldOffset;
        delete tmp->CIVarIndex;
        free(tmp);
        tmp = next;
    }
    for (auto &VarRec : VarByName)
    {
        /* remove any variables that we've created from our IO */
        m_Engine->m_IO.RemoveVariable(VarRec.second->VarName);

        free(VarRec.second->VarName);
        if (VarRec.second->Operator)
            free(VarRec.second->Operator);
        delete VarRec.second;
    }
    if (m_FreeableMBA)
        delete m_FreeableMBA;
    for (auto &step : MetadataBaseArray)
    {
        delete step;
    }
}

void *BP5Deserializer::GetMetadataBase(BP5VarRec *VarRec, size_t Step,
                                       size_t WriterRank) const
{
    MetaArrayRec *writer_meta_base = NULL;
    if (m_RandomAccessMode)
    {
        ControlInfo *CI =
            m_ControlArray[Step][WriterRank]; // writer control array
        if (((*CI->MetaFieldOffset).size() <= VarRec->VarNum) ||
            ((*CI->MetaFieldOffset)[VarRec->VarNum] == 0))
        {
            // Var does not appear in this record
            return NULL;
        }
        size_t CI_VarIndex = (*CI->CIVarIndex)[VarRec->VarNum];
        BP5MetadataInfoStruct *BaseData =
            (BP5MetadataInfoStruct *)(*MetadataBaseArray[Step])[WriterRank];
        if (!BP5BitfieldTest(BaseData, CI_VarIndex))
        {
            // Var appears in CI, but wasn't written on this step
            return NULL;
        }
        size_t MetadataFieldOffset = (*CI->MetaFieldOffset)[VarRec->VarNum];
        writer_meta_base =
            (MetaArrayRec *)(((char *)(*MetadataBaseArray[Step])[WriterRank]) +
                             MetadataFieldOffset);
    }
    else
    {
        if (VarRec->PerWriterMetaFieldOffset[WriterRank] == 0)
        {
            // Writer didn't write this var
            return NULL;
        }
        writer_meta_base =
            (MetaArrayRec *)(((char *)(*m_MetadataBaseAddrs)[WriterRank]) +
                             VarRec->PerWriterMetaFieldOffset[WriterRank]);
    }
    return writer_meta_base;
}

MinVarInfo *BP5Deserializer::MinBlocksInfo(const VariableBase &Var, size_t Step)
{
    BP5VarRec *VarRec = LookupVarByKey((void *)&Var);

    MinVarInfo *MV = new MinVarInfo(VarRec->DimCount, VarRec->GlobalDims);

    const size_t writerCohortSize = WriterCohortSize(Step);
    size_t Id = 0;
    MV->Step = Step;
    MV->Dims = VarRec->DimCount;
    MV->Shape = NULL;
    MV->IsReverseDims =
        ((MV->Dims > 1) && (m_WriterIsRowMajor != m_ReaderIsRowMajor));

    MV->WasLocalValue = (VarRec->OrigShapeID == ShapeID::LocalValue);
    if ((VarRec->OrigShapeID == ShapeID::LocalValue) ||
        (VarRec->OrigShapeID == ShapeID::GlobalValue))
    {
        if (VarRec->OrigShapeID == ShapeID::LocalValue)
        {
            // appear as an array locally
            MV->IsValue = false;
            MV->Dims = 1;
            MV->Shape = (size_t *)writerCohortSize;
        }
        else
        {
            MV->IsValue = true;
        }
        MV->BlocksInfo.reserve(writerCohortSize);

        for (size_t WriterRank = 0; WriterRank < writerCohortSize; WriterRank++)
        {
            MetaArrayRec *writer_meta_base =
                (MetaArrayRec *)GetMetadataBase(VarRec, Step, WriterRank);
            if (writer_meta_base)
            {
                MinBlockInfo Blk;
                Blk.MinMax.Init(VarRec->Type);
                Blk.WriterID = WriterRank;
                Blk.BlockID = Id++;
                Blk.BufferP = writer_meta_base;
                Blk.Start = NULL;
                Blk.Count = NULL;
                if (VarRec->OrigShapeID == ShapeID::LocalValue)
                {
                    Blk.Count = (size_t *)1;
                    Blk.Start = (size_t *)WriterRank;
                }
                if (writer_meta_base)
                {
                    ApplyElementMinMax(Blk.MinMax, VarRec->Type,
                                       writer_meta_base);
                }
                MV->BlocksInfo.push_back(Blk);
            }
        }
        return MV;
    }
    for (size_t WriterRank = 0; WriterRank < writerCohortSize; WriterRank++)
    {
        MetaArrayRec *writer_meta_base =
            (MetaArrayRec *)GetMetadataBase(VarRec, Step, WriterRank);
        if (writer_meta_base)
        {
            if (MV->Shape == NULL)
            {
                MV->Shape = writer_meta_base->Shape;
            }
            size_t WriterBlockCount =
                writer_meta_base->Dims
                    ? writer_meta_base->DBCount / writer_meta_base->Dims
                    : 1;
            Id += WriterBlockCount;
        }
    }
    MV->BlocksInfo.reserve(Id);

    Id = 0;
    for (size_t WriterRank = 0; WriterRank < writerCohortSize; WriterRank++)
    {
        MetaArrayRec *writer_meta_base =
            (MetaArrayRec *)GetMetadataBase(VarRec, Step, WriterRank);

        if (!writer_meta_base)
            continue;
        size_t WriterBlockCount =
            MV->Dims ? writer_meta_base->DBCount / MV->Dims : 1;
        MinMaxStruct *MMs = NULL;
        if (VarRec->MinMaxOffset != SIZE_MAX)
        {
            MMs = *(MinMaxStruct **)(((char *)writer_meta_base) +
                                     VarRec->MinMaxOffset);
        }
        for (size_t i = 0; i < WriterBlockCount; i++)
        {
            size_t *Offsets = NULL;
            size_t *Count = NULL;
            if (writer_meta_base->Offsets)
                Offsets = writer_meta_base->Offsets + (i * MV->Dims);
            if (writer_meta_base->Count)
                Count = writer_meta_base->Count + (i * MV->Dims);
            MinBlockInfo Blk;
            Blk.WriterID = WriterRank;
            Blk.BlockID = Id++;
            Blk.Start = Offsets;
            Blk.Count = Count;
            Blk.MinMax.Init(VarRec->Type);
            if (MMs)
            {

                char *BlockMinAddr =
                    (((char *)MMs) + 2 * i * VarRec->ElementSize);
                char *BlockMaxAddr =
                    (((char *)MMs) + (2 * i + 1) * VarRec->ElementSize);
                ApplyElementMinMax(Blk.MinMax, VarRec->Type,
                                   (void *)BlockMinAddr);
                ApplyElementMinMax(Blk.MinMax, VarRec->Type,
                                   (void *)BlockMaxAddr);
            }
            // Blk.BufferP
            MV->BlocksInfo.push_back(Blk);
        }
    }
    return MV;
}

static void ApplyElementMinMax(MinMaxStruct &MinMax, DataType Type,
                               void *Element)
{
    switch (Type)
    {
    case DataType::None:
        break;
    case DataType::Char:
    case DataType::Int8:
        if (*(int8_t *)Element < MinMax.MinUnion.field_int8)
            MinMax.MinUnion.field_int8 = *(int8_t *)Element;
        if (*(int8_t *)Element > MinMax.MaxUnion.field_int8)
            MinMax.MaxUnion.field_int8 = *(int8_t *)Element;
        break;
    case DataType::Int16:
        if (*(int16_t *)Element < MinMax.MinUnion.field_int16)
            MinMax.MinUnion.field_int16 = *(int16_t *)Element;
        if (*(int16_t *)Element > MinMax.MaxUnion.field_int16)
            MinMax.MaxUnion.field_int16 = *(int16_t *)Element;
        break;
    case DataType::Int32:
        if (*(int32_t *)Element < MinMax.MinUnion.field_int32)
            MinMax.MinUnion.field_int32 = *(int32_t *)Element;
        if (*(int32_t *)Element > MinMax.MaxUnion.field_int32)
            MinMax.MaxUnion.field_int32 = *(int32_t *)Element;
        break;
    case DataType::Int64:
        if (*(int64_t *)Element < MinMax.MinUnion.field_int64)
            MinMax.MinUnion.field_int64 = *(int64_t *)Element;
        if (*(int64_t *)Element > MinMax.MaxUnion.field_int64)
            MinMax.MaxUnion.field_int64 = *(int64_t *)Element;
        break;
    case DataType::UInt8:
        if (*(uint8_t *)Element < MinMax.MinUnion.field_uint8)
            MinMax.MinUnion.field_uint8 = *(uint8_t *)Element;
        if (*(uint8_t *)Element > MinMax.MaxUnion.field_uint8)
            MinMax.MaxUnion.field_uint8 = *(uint8_t *)Element;
        break;
    case DataType::UInt16:
        if (*(uint16_t *)Element < MinMax.MinUnion.field_uint16)
            MinMax.MinUnion.field_uint16 = *(uint16_t *)Element;
        if (*(uint16_t *)Element > MinMax.MaxUnion.field_uint16)
            MinMax.MaxUnion.field_uint16 = *(uint16_t *)Element;
        break;
    case DataType::UInt32:
        if (*(uint32_t *)Element < MinMax.MinUnion.field_uint32)
            MinMax.MinUnion.field_uint32 = *(uint32_t *)Element;
        if (*(uint32_t *)Element > MinMax.MaxUnion.field_uint32)
            MinMax.MaxUnion.field_uint32 = *(uint32_t *)Element;
        break;
    case DataType::UInt64:
        if (*(uint64_t *)Element < MinMax.MinUnion.field_uint64)
            MinMax.MinUnion.field_uint64 = *(uint64_t *)Element;
        if (*(uint64_t *)Element > MinMax.MaxUnion.field_uint64)
            MinMax.MaxUnion.field_uint64 = *(uint64_t *)Element;
        break;
    case DataType::Float:
        if (*(float *)Element < MinMax.MinUnion.field_float)
            MinMax.MinUnion.field_float = *(float *)Element;
        if (*(float *)Element > MinMax.MaxUnion.field_float)
            MinMax.MaxUnion.field_float = *(float *)Element;
        break;
    case DataType::Double:
        if (*(double *)Element < MinMax.MinUnion.field_double)
            MinMax.MinUnion.field_double = *(double *)Element;
        if (*(double *)Element > MinMax.MaxUnion.field_double)
            MinMax.MaxUnion.field_double = *(double *)Element;
        break;
    case DataType::LongDouble:
        if (*(long double *)Element < MinMax.MinUnion.field_ldouble)
            MinMax.MinUnion.field_ldouble = *(long double *)Element;
        if (*(long double *)Element > MinMax.MaxUnion.field_ldouble)
            MinMax.MaxUnion.field_ldouble = *(long double *)Element;
        break;
    case DataType::FloatComplex:
    case DataType::DoubleComplex:
    case DataType::String:
    case DataType::Struct:
        break;
    }
}

size_t BP5Deserializer::RelativeToAbsoluteStep(const BP5VarRec *VarRec,
                                               size_t RelStep)
{
    //  Consider an optimization here.  Track the number of timesteps
    //  available to the engine and the number of steps upon which a
    //  variable appears.  If the first step it appears on plus the
    //  number of steps it appears adds up to the number of steps
    //  available to the engine, then there are no gaps and we can
    //  easily calculate the RelativeToAbsoluteStep transformation
    //  without checking.  That's probably the most common case.
    //  But for now, the simple stupid solution
    size_t AbsStep = VarRec->FirstTSSeen;
    while (RelStep != 0)
    {
        size_t WriterRank = 0;
        const size_t writerCohortSize = WriterCohortSize(AbsStep);
        while (WriterRank < writerCohortSize)
        {
            BP5MetadataInfoStruct *BaseData;
            BaseData = (BP5MetadataInfoStruct
                            *)(*MetadataBaseArray[AbsStep])[WriterRank];
            if (BP5BitfieldTest((BP5MetadataInfoStruct *)BaseData,
                                VarRec->VarNum))
            {
                // variable appeared on this step
                RelStep--;
                break; // exit while (WriterRank < writerCohortSize)
            }
            WriterRank++;
        }
        AbsStep++;
    }
    return AbsStep;
}

void BP5Deserializer::GetAbsoluteSteps(const VariableBase &Var,
                                       std::vector<size_t> &keys) const
{
    BP5VarRec *VarRec = LookupVarByKey((void *)&Var);
    if (!m_RandomAccessMode)
        return;

    for (size_t Step = 0; Step < m_ControlArray.size(); Step++)
    {
        for (size_t WriterRank = 0; WriterRank < WriterCohortSize(Step);
             WriterRank++)
        {
            MetaArrayRec *writer_meta_base =
                (MetaArrayRec *)GetMetadataBase(VarRec, Step, WriterRank);
            if (writer_meta_base)
            {
                keys.push_back(Step);
                break;
            }
        }
    }
}

Dims *BP5Deserializer::VarShape(const VariableBase &Var,
                                const size_t RelStep) const
{
    BP5VarRec *VarRec = LookupVarByKey((void *)&Var);
    if (VarRec->OrigShapeID != ShapeID::GlobalArray)
    {
        return nullptr;
    }
    size_t AbsStep = RelStep;
    if (m_RandomAccessMode)
    {
        if (RelStep == adios2::EngineCurrentStep)
        {
            AbsStep = VarRec->AbsStepFromRel[Var.m_StepsStart];
        }
        else
        {
            AbsStep = VarRec->AbsStepFromRel[RelStep];
        }
    }
    for (size_t WriterRank = 0; WriterRank < WriterCohortSize(AbsStep);
         WriterRank++)
    {
        MetaArrayRec *writer_meta_base =
            (MetaArrayRec *)GetMetadataBase(VarRec, AbsStep, WriterRank);
        if (writer_meta_base && writer_meta_base->Shape)
        {
            Dims *Shape = new Dims();
            Shape->reserve(writer_meta_base->Dims);
            for (size_t i = 0; i < writer_meta_base->Dims; i++)
            {
                Shape->push_back(writer_meta_base->Shape[i]);
            }
            return Shape;
        }
    }
    return nullptr;
}

bool BP5Deserializer::VariableMinMax(const VariableBase &Var, const size_t Step,
                                     MinMaxStruct &MinMax)
{
    BP5VarRec *VarRec = LookupVarByKey((void *)&Var);
    if ((VarRec->OrigShapeID == ShapeID::LocalArray) ||
        (VarRec->OrigShapeID == ShapeID::GlobalArray))
    {
        if (VarRec->MinMaxOffset == SIZE_MAX)
        {
            std::memset(&MinMax, 0, sizeof(struct MinMaxStruct));
            return true;
        }
    }

    MinMax.Init(VarRec->Type);

    const size_t writerCohortSize = WriterCohortSize(Step);
    size_t StartStep = Step, StopStep = Step + 1;
    if (Step == DefaultSizeT)
    {
        StartStep = 0;
        StopStep = m_ControlArray.size();
        if (!m_RandomAccessMode)
            StopStep = 1;
    }
    for (size_t RelStep = StartStep; RelStep < StopStep; RelStep++)
    {
        if ((VarRec->OrigShapeID == ShapeID::LocalArray) ||
            (VarRec->OrigShapeID == ShapeID::GlobalArray))
        {
            for (size_t WriterRank = 0; WriterRank < writerCohortSize;
                 WriterRank++)
            {
                MetaArrayRec *writer_meta_base =
                    (MetaArrayRec *)GetMetadataBase(VarRec, RelStep,
                                                    WriterRank);

                if (!writer_meta_base)
                    continue;
                size_t WriterBlockCount =
                    VarRec->DimCount
                        ? writer_meta_base->DBCount / VarRec->DimCount
                        : 1;
                for (size_t B = 0; B < WriterBlockCount; B++)
                {
                    void *MMs = *(void **)(((char *)writer_meta_base) +
                                           VarRec->MinMaxOffset);
                    ApplyElementMinMax(
                        MinMax, VarRec->Type,
                        (void *)(((char *)MMs) + 2 * B * Var.m_ElementSize));
                    ApplyElementMinMax(
                        MinMax, VarRec->Type,
                        (void *)(((char *)MMs) +
                                 (2 * B + 1) * Var.m_ElementSize));
                }
            }
        }
        else if (VarRec->OrigShapeID == ShapeID::GlobalValue)
        {
            void *writer_meta_base = NULL;
            size_t WriterRank = 0;
            while ((writer_meta_base == NULL) &&
                   (WriterRank < writerCohortSize))
            {
                writer_meta_base =
                    GetMetadataBase(VarRec, RelStep, WriterRank++);
            }
            if (writer_meta_base)
            {
                ApplyElementMinMax(MinMax, VarRec->Type, writer_meta_base);
            }
        }
        else if (VarRec->OrigShapeID == ShapeID::LocalValue)
        {
            for (size_t WriterRank = 0; WriterRank < writerCohortSize;
                 WriterRank++)
            {
                void *writer_meta_base =
                    GetMetadataBase(VarRec, RelStep, WriterRank);
                if (writer_meta_base)
                {
                    ApplyElementMinMax(MinMax, VarRec->Type, writer_meta_base);
                }
            }
        }
    }
    return true;
}

}
}
