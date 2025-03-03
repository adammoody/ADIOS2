/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BP5Serializer.cpp
 *
 */

#include "adios2/core/Attribute.h"
#include "adios2/core/Engine.h"
#include "adios2/core/IO.h"
#include "adios2/core/VariableBase.h"
#include "adios2/helper/adiosFunctions.h"
#include "adios2/helper/adiosMemory.h"
#include "adios2/toolkit/format/buffer/ffs/BufferFFS.h"

#include <stddef.h> // max_align_t

#include <cstring>

#include "BP5Serializer.h"

#ifdef _WIN32
#pragma warning(disable : 4250)
#endif

namespace adios2
{
namespace format
{

BP5Serializer::BP5Serializer() { Init(); }
BP5Serializer::~BP5Serializer()
{
    if (Info.RecList)
    {
        for (int i = 0; i < Info.RecCount; i++)
        {
            if (Info.RecList[i].OperatorType)
                free(Info.RecList[i].OperatorType);
        }
        free(Info.RecList);
    }
    if (Info.MetaFieldCount)
        free_FMfield_list(Info.MetaFields);
    if (Info.LocalFMContext)
        free_FMcontext(Info.LocalFMContext);
    if (Info.AttributeFields)
        free_FMfield_list(Info.AttributeFields);
    if (Info.AttributeData)
        free(Info.AttributeData);
    if (MetadataBuf)
    {
        if (((BP5MetadataInfoStruct *)MetadataBuf)->BitField)
            free(((BP5MetadataInfoStruct *)MetadataBuf)->BitField);
        free(MetadataBuf);
    }
}

void BP5Serializer::Init()
{
    // Re-init Info to zero
    Info = FFSWriterMarshalBase();
    Info.RecCount = 0;
    Info.RecList = (BP5Serializer::BP5WriterRec)malloc(sizeof(Info.RecList[0]));
    Info.MetaFieldCount = 0;
    Info.MetaFields = NULL;
    Info.LocalFMContext = create_local_FMcontext();
    AddSimpleField(&Info.MetaFields, &Info.MetaFieldCount, "BitFieldCount",
                   "integer", sizeof(size_t));
    AddSimpleField(&Info.MetaFields, &Info.MetaFieldCount, "BitField",
                   "integer[BitFieldCount]", sizeof(size_t));
    AddSimpleField(&Info.MetaFields, &Info.MetaFieldCount, "DataBlockSize",
                   "integer", sizeof(size_t));
    RecalcMarshalStorageSize();

    ((BP5MetadataInfoStruct *)MetadataBuf)->BitFieldCount = 0;
    ((BP5MetadataInfoStruct *)MetadataBuf)->BitField =
        (std::size_t *)malloc(sizeof(size_t));
    ((BP5MetadataInfoStruct *)MetadataBuf)->DataBlockSize = 0;
}
BP5Serializer::BP5WriterRec BP5Serializer::LookupWriterRec(void *Key)
{
    for (int i = 0; i < Info.RecCount; i++)
    {
        if (Info.RecList[i].Key == Key)
        {
            return &Info.RecList[i];
        }
    }

    return NULL;
}

void BP5Serializer::RecalcMarshalStorageSize()
{
    if (Info.MetaFieldCount)
    {
        FMFieldList LastMetaField;
        size_t NewMetaSize;
        LastMetaField = &Info.MetaFields[Info.MetaFieldCount - 1];
        NewMetaSize =
            (LastMetaField->field_offset + LastMetaField->field_size + 7) & ~7;
        MetadataBuf = realloc(MetadataBuf, NewMetaSize + 8);
        memset((char *)(MetadataBuf) + MetadataSize, 0,
               NewMetaSize - MetadataSize);
        MetadataSize = NewMetaSize;
    }
}

void BP5Serializer::RecalcAttributeStorageSize()
{
    if (Info.AttributeFieldCount)
    {
        FMFieldList LastAttributeField;
        size_t NewAttributeSize;
        LastAttributeField =
            &Info.AttributeFields[Info.AttributeFieldCount - 1];
        NewAttributeSize = (LastAttributeField->field_offset +
                            LastAttributeField->field_size + 7) &
                           ~7;
        Info.AttributeData = realloc(Info.AttributeData, NewAttributeSize + 8);
        memset((char *)(Info.AttributeData) + Info.AttributeSize, 0,
               NewAttributeSize - Info.AttributeSize);
        Info.AttributeSize = NewAttributeSize;
    }
}

void BP5Serializer::AddSimpleField(FMFieldList *FieldP, int *CountP,
                                   const char *Name, const char *Type,
                                   int ElementSize)
{
    int Offset = 0;
    FMFieldList Field;
    if (*CountP)
    {
        FMFieldList PriorField;
        PriorField = &((*FieldP)[(*CountP) - 1]);
        int PriorFieldSize = PriorField->field_size;
        if (strchr(PriorField->field_type, '['))
        {
            // really a pointer
            PriorFieldSize = sizeof(void *);
        }
        Offset =
            ((PriorField->field_offset + PriorFieldSize + ElementSize - 1) /
             ElementSize) *
            ElementSize;
    }
    if (*FieldP)
        *FieldP =
            (FMFieldList)realloc(*FieldP, (*CountP + 2) * sizeof((*FieldP)[0]));
    else
        *FieldP = (FMFieldList)malloc((*CountP + 2) * sizeof((*FieldP)[0]));

    Field = &((*FieldP)[*CountP]);
    (*CountP)++;
    Field->field_name = strdup(Name);
    Field->field_type = strdup(Type);
    Field->field_size = ElementSize;
    Field->field_offset = Offset;
    Field++;
    Field->field_name = NULL;
    Field->field_type = NULL;
    Field->field_size = 0;
    Field->field_offset = 0;
}

typedef struct dcomplex
{
    double real_part;
    double imag_part;
} dcomplex_struct;

typedef struct fcomplex
{
    float real_part;
    float imag_part;
} fcomplex_struct;

FMField fcomplex_field_list[] = {
    {"real", "float", sizeof(float), FMOffset(fcomplex_struct *, real_part)},
    {"imag", "float", sizeof(float), FMOffset(fcomplex_struct *, imag_part)},
    {NULL, NULL, 0, 0}};

FMField dcomplex_field_list[] = {
    {"real", "float", sizeof(double), FMOffset(dcomplex_struct *, real_part)},
    {"imag", "float", sizeof(double), FMOffset(dcomplex_struct *, imag_part)},
    {NULL, NULL, 0, 0}};

static const char *NamePrefix(ShapeID Shape)
{
    const char *Prefix = "BP5";
    switch (Shape)
    {
    case ShapeID::Unknown:
        Prefix = "BPU";
        break;
    case ShapeID::GlobalValue:
        Prefix = "BPg";
        break;
    case ShapeID::GlobalArray:
        Prefix = "BPG";
        break;
    case ShapeID::JoinedArray:
        Prefix = "BPJ";
        break;
    case ShapeID::LocalValue:
        Prefix = "BPl";
        break;
    case ShapeID::LocalArray:
        Prefix = "BPL";
        break;
    }
    return Prefix;
}

static char *ConcatName(const char *base_name, const char *postfix)
{
    char *Ret = (char *)malloc(strlen(base_name) + strlen(postfix) + 2);
    strcpy(Ret, base_name);
    strcat(Ret, "_");
    strcat(Ret, postfix);
    return Ret;
}

char *BP5Serializer::BuildVarName(const char *base_name, const ShapeID Shape,
                                  const int type, const int element_size)
{

    const char *Prefix = NamePrefix(Shape);
    int Len = strlen(base_name) + 2 + strlen(Prefix) + 16;
    char *Ret = (char *)malloc(Len);
    if (element_size == 0)
    {
        strcpy(Ret, Prefix);
        strcat(Ret, "_");
        strcat(Ret, base_name);
    }
    else
    {
        sprintf(Ret, "%s_%d_%d_", Prefix, element_size, type);
        strcat(Ret, base_name);
    }
    return Ret;
}

static char *BuildLongName(const char *base_name, const ShapeID Shape,
                           const int type, const int element_size)
{
    const char *Prefix = NamePrefix(Shape);
    int Len = strlen(base_name) + 3 + strlen(Prefix) + 16;
    char *Ret = (char *)malloc(Len);
    sprintf(Ret, "%s_%d_%d", Prefix, element_size, type);
    strcat(Ret, "_");
    strcat(Ret, base_name);
    return Ret;
}

void BP5Serializer::BreakdownVarName(const char *Name, char **base_name_p,
                                     int *type_p, int *element_size_p)
{
    int Type;
    int ElementSize;
    const char *NameStart = strchr(strchr(Name, '_') + 1, '_') + 1;
    sscanf(Name + 3, "%d_%d_", &ElementSize, &Type);
    *element_size_p = ElementSize;
    *type_p = Type;
    *base_name_p = strdup(NameStart);
}

char *BP5Serializer::BuildArrayDimsName(const char *base_name, const int type,
                                        const int element_size)
{
    const char *Prefix = NamePrefix(ShapeID::GlobalArray);
    int Len = strlen(base_name) + 3 + strlen(Prefix) + 16;
    char *Ret = (char *)malloc(Len);
    sprintf(Ret, "%s%d_%d_", Prefix, element_size, type);
    strcat(Ret, base_name);
    strcat(Ret, "Dims");
    return Ret;
}

char *BP5Serializer::BuildArrayDBCountName(const char *base_name,
                                           const int type,
                                           const int element_size)
{
    const char *Prefix = NamePrefix(ShapeID::GlobalArray);
    int Len = strlen(base_name) + 3 + strlen(Prefix) + 16;
    char *Ret = (char *)malloc(Len);
    sprintf(Ret, "%s%d_%d_", Prefix, element_size, type);
    strcat(Ret, base_name);
    strcat(Ret, "DBCount");
    return Ret;
}

char *BP5Serializer::BuildArrayBlockCountName(const char *base_name,
                                              const int type,
                                              const int element_size)
{
    const char *Prefix = NamePrefix(ShapeID::GlobalArray);
    int Len = strlen(base_name) + 3 + strlen(Prefix) + 24;
    char *Ret = (char *)malloc(Len);
    sprintf(Ret, "%s%d_%d_", Prefix, element_size, type);
    strcat(Ret, base_name);
    strcat(Ret, "BlockCount");
    return Ret;
}

char *BP5Serializer::TranslateADIOS2Type2FFS(const DataType Type)
{
    switch (Type)
    {
    case DataType::None:
    case DataType::Struct:
        return NULL;
    case DataType::Int8:
    case DataType::Int16:
    case DataType::Int32:
    case DataType::Int64:
    case DataType::Char:
        return strdup("integer");
    case DataType::UInt8:
    case DataType::UInt16:
    case DataType::UInt32:
    case DataType::UInt64:
        return strdup("unsigned integer");
    case DataType::Float:
    case DataType::Double:
    case DataType::LongDouble:
        return strdup("float");
    case DataType::FloatComplex:
        return strdup("complex4");
    case DataType::DoubleComplex:
        return strdup("complex8");
    case DataType::String:
        return strdup("string");
    }
    return 0;
}

void BP5Serializer::AddField(FMFieldList *FieldP, int *CountP, const char *Name,
                             const DataType Type, int ElementSize)
{
    char *TransType = TranslateADIOS2Type2FFS(Type);
    AddSimpleField(FieldP, CountP, Name, TransType, ElementSize);
    free(TransType);
}

void BP5Serializer::AddFixedArrayField(FMFieldList *FieldP, int *CountP,
                                       const char *Name, const DataType Type,
                                       int ElementSize, int DimCount)
{
    const char *TransType = TranslateADIOS2Type2FFS(Type);
    char *TypeWithArray = (char *)malloc(strlen(TransType) + 16);
    sprintf(TypeWithArray, "*(%s[%d])", TransType, DimCount);
    free((void *)TransType);
    AddSimpleField(FieldP, CountP, Name, TypeWithArray, sizeof(void *));
    free(TypeWithArray);
    (*FieldP)[*CountP - 1].field_size = ElementSize;
}

void BP5Serializer::AddVarArrayField(FMFieldList *FieldP, int *CountP,
                                     const char *Name, const DataType Type,
                                     int ElementSize, char *SizeField)
{
    char *TransType = TranslateADIOS2Type2FFS(Type);
    char *TypeWithArray =
        (char *)malloc(strlen(TransType) + strlen(SizeField) + 8);
    sprintf(TypeWithArray, "%s[%s]", TransType, SizeField);
    free(TransType);
    AddSimpleField(FieldP, CountP, Name, TypeWithArray, sizeof(void *));
    free(TypeWithArray);
    (*FieldP)[*CountP - 1].field_size = ElementSize;
}

void BP5Serializer::AddDoubleArrayField(FMFieldList *FieldP, int *CountP,
                                        const char *Name, const DataType Type,
                                        int ElementSize, char *SizeField)
{
    char *TransType = TranslateADIOS2Type2FFS(Type);
    char *TypeWithArray =
        (char *)malloc(strlen(TransType) + strlen(SizeField) + 8);
    sprintf(TypeWithArray, "%s[2][%s]", TransType, SizeField);
    AddSimpleField(FieldP, CountP, Name, TypeWithArray, sizeof(void *));
    free(TransType);
    free(TypeWithArray);
    (*FieldP)[*CountP - 1].field_size = ElementSize;
}

BP5Serializer::BP5WriterRec
BP5Serializer::CreateWriterRec(void *Variable, const char *Name, DataType Type,
                               size_t ElemSize, size_t DimCount)
{
    core::VariableBase *VB = static_cast<core::VariableBase *>(Variable);
    Info.RecList = (BP5WriterRec)realloc(
        Info.RecList, (Info.RecCount + 1) * sizeof(Info.RecList[0]));
    BP5WriterRec Rec = &Info.RecList[Info.RecCount];
    if (Type == DataType::String)
        ElemSize = sizeof(char *);
    Rec->Key = Variable;
    Rec->FieldID = Info.RecCount;
    Rec->DimCount = DimCount;
    Rec->Type = (int)Type;
    Rec->OperatorType = NULL;
    if (DimCount == 0)
    {
        // simple field, only add base value FMField to metadata
        char *SstName = BuildVarName(Name, VB->m_ShapeID, 0,
                                     0); // size and type in full field spec
        AddField(&Info.MetaFields, &Info.MetaFieldCount, SstName, Type,
                 ElemSize);
        free(SstName);
        RecalcMarshalStorageSize();
        Rec->MetaOffset = Info.MetaFields[Info.MetaFieldCount - 1].field_offset;
        Rec->DataOffset = (size_t)-1;
        // Changing the formats renders these invalid
        Info.MetaFormat = NULL;
    }
    else
    {
        char *OperatorType = NULL;
        if (VB->m_Operations.size())
        {
            OperatorType = strdup((VB->m_Operations[0])->m_TypeString.data());
        }
        // Array field.  To Metadata, add FMFields for DimCount, Shape, Count
        // and Offsets matching _MetaArrayRec
        char *LongName =
            BuildLongName(Name, VB->m_ShapeID, (int)Type, ElemSize);

        const char *ArrayTypeName = "MetaArray";
        int FieldSize = sizeof(MetaArrayRec);
        if (VB->m_Operations.size())
        {
            ArrayTypeName = "MetaArrayOp";
            FieldSize = sizeof(MetaArrayRecOperator);
        }
        if (m_StatsLevel > 0)
        {
            char MMArrayName[40] = {0};
            strcat(MMArrayName, ArrayTypeName);
            switch (ElemSize)
            {
            case 1:
                strcat(MMArrayName, "MM1");
                break;
            case 2:
                strcat(MMArrayName, "MM2");
                break;
            case 4:
                strcat(MMArrayName, "MM4");
                break;
            case 8:
                strcat(MMArrayName, "MM8");
                break;
            case 16:
                strcat(MMArrayName, "MM16");
                break;
            }
            Rec->MinMaxOffset = FieldSize;
            FieldSize += sizeof(char *);
            AddSimpleField(&Info.MetaFields, &Info.MetaFieldCount, LongName,
                           MMArrayName, FieldSize);
        }
        else
        {
            AddSimpleField(&Info.MetaFields, &Info.MetaFieldCount, LongName,
                           ArrayTypeName, FieldSize);
        }
        Rec->MetaOffset = Info.MetaFields[Info.MetaFieldCount - 1].field_offset;
        Rec->OperatorType = OperatorType;
        free(LongName);
        RecalcMarshalStorageSize();

        // Changing the formats renders these invalid
        Info.MetaFormat = NULL;
    }
    Info.RecCount++;
    return Rec;
}

size_t *BP5Serializer::CopyDims(const size_t Count, const size_t *Vals)
{
    size_t *Ret = (size_t *)malloc(Count * sizeof(Ret[0]));
    memcpy(Ret, Vals, Count * sizeof(Ret[0]));
    return Ret;
}

size_t *BP5Serializer::AppendDims(size_t *OldDims, const size_t OldCount,
                                  const size_t Count, const size_t *Vals)
{
    size_t *Ret =
        (size_t *)realloc(OldDims, (OldCount + Count) * sizeof(Ret[0]));
    memcpy(Ret + OldCount, Vals, Count * sizeof(Ret[0]));
    return Ret;
}

size_t BP5Serializer::CalcSize(const size_t Count, const size_t *Vals)
{
    size_t i;
    size_t Elems = 1;
    for (i = 0; i < Count; i++)
    {
        Elems *= Vals[i];
    }
    return Elems;
}

void BP5Serializer::PerformPuts(bool forceCopyDeferred)
{
    // Copy all data for externs into iovec
    DumpDeferredBlocks(true);
}

void BP5Serializer::DumpDeferredBlocks(bool forceCopyDeferred)
{
    for (auto &Def : DeferredExterns)
    {
        MetaArrayRec *MetaEntry =
            (MetaArrayRec *)((char *)(MetadataBuf) + Def.MetaOffset);
        size_t DataOffset =
            m_PriorDataBufferSizeTotal +
            CurDataBuffer->AddToVec(Def.DataSize, Def.Data, Def.AlignReq,
                                    forceCopyDeferred);
        MetaEntry->DataBlockLocation[Def.BlockID] = DataOffset;
    }
    DeferredExterns.clear();
}

static void GetMinMax(const void *Data, size_t ElemCount, const DataType Type,
                      MinMaxStruct &MinMax, MemorySpace MemSpace)
{
    MinMax.Init(Type);
    if (ElemCount == 0)
        return;
    if (Type == DataType::Struct)
    {
    }
#ifdef ADIOS2_HAVE_CUDA
#define pertype(T, N)                                                          \
    else if (MemSpace == MemorySpace::CUDA &&                                  \
             Type == helper::GetDataType<T>())                                 \
    {                                                                          \
        const T *values = (const T *)Data;                                     \
        helper::CUDAMinMax(values, ElemCount, MinMax.MinUnion.field_##N,       \
                           MinMax.MaxUnion.field_##N);                         \
    }
    ADIOS2_FOREACH_MINMAX_STDTYPE_2ARGS(pertype)
#undef pertype
#endif
#define pertype(T, N)                                                          \
    else if (Type == helper::GetDataType<T>())                                 \
    {                                                                          \
        const T *values = (const T *)Data;                                     \
        auto res = std::minmax_element(values, values + ElemCount);            \
        MinMax.MinUnion.field_##N = *res.first;                                \
        MinMax.MaxUnion.field_##N = *res.second;                               \
    }
    ADIOS2_FOREACH_MINMAX_STDTYPE_2ARGS(pertype)
}

void BP5Serializer::Marshal(void *Variable, const char *Name,
                            const DataType Type, size_t ElemSize,
                            size_t DimCount, const size_t *Shape,
                            const size_t *Count, const size_t *Offsets,
                            const void *Data, bool Sync,
                            BufferV::BufferPos *Span)
{

    core::VariableBase *VB = static_cast<core::VariableBase *>(Variable);

    BP5MetadataInfoStruct *MBase;

    BP5WriterRec Rec = LookupWriterRec(Variable);

    bool DeferAddToVec;

    if (VB->m_SingleValue)
    {
        DimCount = 0;
    }
    if (!Rec)
    {
        Rec = CreateWriterRec(Variable, Name, Type, ElemSize, DimCount);
    }

    if (!Sync && (Rec->DimCount != 0) && !Span && !Rec->OperatorType)
    {
        /*
         * If this is a big external block, we'll do everything except add it to
         * the BufferV now, saving enough information to add it and patch back
         * the DataBlockLocation in the metadata in DumpDeferredBlocks()
         */
        DeferAddToVec = true;
    }
    else
    {
        /*
         * If there is an operator, or if it's a span put, or a sync put, or if
         * the block is smallish and we might as well copy it now, we want to
         * allocate internal memory at this point.
         */
        DeferAddToVec = false;
    }

    MBase = (struct BP5MetadataInfoStruct *)MetadataBuf;
    int AlreadyWritten = BP5BitfieldTest(MBase, Rec->FieldID);
    BP5BitfieldSet(MBase, Rec->FieldID);

    if (VB->m_SingleValue)
    {
        if (Type != DataType::String)
            memcpy((char *)(MetadataBuf) + Rec->MetaOffset, Data, ElemSize);
        else
        {
            char **StrPtr = (char **)((char *)(MetadataBuf) + Rec->MetaOffset);
            if (AlreadyWritten && (*StrPtr != NULL))
                free(*StrPtr);
            *StrPtr = strdup(*(char **)Data);
        }
    }
    else
    {
        MemorySpace MemSpace = MemorySpace::Host;
        if (VB->IsCUDAPointer(Data))
            MemSpace = MemorySpace::CUDA;
        MetaArrayRec *MetaEntry =
            (MetaArrayRec *)((char *)(MetadataBuf) + Rec->MetaOffset);
        size_t ElemCount = CalcSize(DimCount, Count);
        size_t DataOffset = 0;
        size_t CompressedSize = 0;
        /* handle metadata */
        MetaEntry->Dims = DimCount;
        if (CurDataBuffer == NULL)
        {
            helper::Throw<std::logic_error>("Toolkit", "format::BP5Serializer",
                                            "Marshal", "without prior Init");
        }

        MinMaxStruct MinMax;
        MinMax.Init(Type);
        if ((m_StatsLevel > 0) && !Span)
        {
            GetMinMax(Data, ElemCount, (DataType)Rec->Type, MinMax, MemSpace);
        }

        if (Rec->OperatorType)
        {
            std::string compressionMethod = Rec->OperatorType;
            std::transform(compressionMethod.begin(), compressionMethod.end(),
                           compressionMethod.begin(), ::tolower);
            Dims tmpCount, tmpOffsets;
            for (size_t i = 0; i < DimCount; i++)
            {
                tmpCount.push_back(Count[i]);
                tmpOffsets.push_back(Offsets[i]);
            }
            size_t AllocSize = ElemCount * ElemSize + 100;
            BufferV::BufferPos pos =
                CurDataBuffer->Allocate(AllocSize, ElemSize);
            char *CompressedData =
                (char *)GetPtr(pos.bufferIdx, pos.posInBuffer);
            DataOffset = m_PriorDataBufferSizeTotal + pos.globalPos;
            CompressedSize = VB->m_Operations[0]->Operate(
                (const char *)Data, tmpOffsets, tmpCount, (DataType)Rec->Type,
                CompressedData);
            CurDataBuffer->DownsizeLastAlloc(AllocSize, CompressedSize);
        }
        else if (Span == nullptr)
        {
            if (!DeferAddToVec)
            {
                DataOffset = m_PriorDataBufferSizeTotal +
                             CurDataBuffer->AddToVec(ElemCount * ElemSize, Data,
                                                     ElemSize, Sync, MemSpace);
            }
        }
        else
        {
            *Span = CurDataBuffer->Allocate(ElemCount * ElemSize, ElemSize);
            DataOffset = m_PriorDataBufferSizeTotal + Span->globalPos;
        }

        if (!AlreadyWritten)
        {
            if (Shape)
                MetaEntry->Shape = CopyDims(DimCount, Shape);
            else
                MetaEntry->Shape = NULL;
            MetaEntry->DBCount = DimCount;
            MetaEntry->Count = CopyDims(DimCount, Count);
            MetaEntry->BlockCount = 1;
            MetaEntry->DataBlockLocation = (size_t *)malloc(sizeof(size_t));
            MetaEntry->DataBlockLocation[0] = DataOffset;
            if (Rec->OperatorType)
            {
                MetaArrayRecOperator *OpEntry =
                    (MetaArrayRecOperator *)MetaEntry;
                OpEntry->DataBlockSize = (size_t *)malloc(sizeof(size_t));
                OpEntry->DataBlockSize[0] = CompressedSize;
            }
            if (Offsets)
                MetaEntry->Offsets = CopyDims(DimCount, Offsets);
            else
                MetaEntry->Offsets = NULL;
            if (m_StatsLevel > 0)
            {
                void **MMPtrLoc =
                    (void **)(((char *)MetaEntry) + Rec->MinMaxOffset);
                *MMPtrLoc = (void *)malloc(ElemSize * 2);
                memcpy(*MMPtrLoc, &MinMax.MinUnion, ElemSize);
                memcpy(((char *)*MMPtrLoc) + ElemSize, &MinMax.MaxUnion,
                       ElemSize);
            }
            if (DeferAddToVec)
            {
                DeferredExtern rec = {Rec->MetaOffset, 0, Data,
                                      ElemCount * ElemSize, ElemSize};
                DeferredExterns.push_back(rec);
            }
        }
        else
        {
            /* already got some metadata, add blocks */
            size_t PreviousDBCount = MetaEntry->DBCount;
            //  Assume shape is still valid   (modify this if shape /global
            //  dimensions can change )
            // Also assume Dims is always right and consistent, otherwise,
            // bad things
            if (Shape && MetaEntry->Shape)
            {
                // Shape can change with later writes, so must overwrite
                memcpy(MetaEntry->Shape, Shape, DimCount * sizeof(Shape[0]));
            }
            MetaEntry->DBCount += DimCount;
            MetaEntry->BlockCount++;
            MetaEntry->Count =
                AppendDims(MetaEntry->Count, PreviousDBCount, DimCount, Count);
            MetaEntry->DataBlockLocation =
                (size_t *)realloc(MetaEntry->DataBlockLocation,
                                  MetaEntry->BlockCount * sizeof(size_t));
            MetaEntry->DataBlockLocation[MetaEntry->BlockCount - 1] =
                DataOffset;
            if (Rec->OperatorType)
            {
                MetaArrayRecOperator *OpEntry =
                    (MetaArrayRecOperator *)MetaEntry;
                OpEntry->DataBlockSize =
                    (size_t *)realloc(OpEntry->DataBlockSize,
                                      OpEntry->BlockCount * sizeof(size_t));
                OpEntry->DataBlockSize[OpEntry->BlockCount - 1] =
                    CompressedSize;
            }
            if (m_StatsLevel > 0)
            {
                void **MMPtrLoc =
                    (void **)(((char *)MetaEntry) + Rec->MinMaxOffset);
                *MMPtrLoc = (void *)realloc(*MMPtrLoc, MetaEntry->BlockCount *
                                                           ElemSize * 2);
                memcpy(((char *)*MMPtrLoc) +
                           ElemSize * (2 * (MetaEntry->BlockCount - 1)),
                       &MinMax.MinUnion, ElemSize);
                memcpy(((char *)*MMPtrLoc) +
                           ElemSize * (2 * (MetaEntry->BlockCount - 1) + 1),
                       &MinMax.MaxUnion, ElemSize);
            }
            if (DeferAddToVec)
            {
                DeferredExterns.push_back({Rec->MetaOffset,
                                           MetaEntry->BlockCount - 1, Data,
                                           ElemCount * ElemSize, ElemSize});
            }
            if (Offsets)
                MetaEntry->Offsets = AppendDims(
                    MetaEntry->Offsets, PreviousDBCount, DimCount, Offsets);
        }
    }
}

void BP5Serializer::MarshalAttribute(const char *Name, const DataType Type,
                                     size_t ElemSize, size_t ElemCount,
                                     const void *Data)
{

    const char *AttrString = NULL;
    const void *DataAddress = Data;

    NewAttribute = true;
    if (Type == DataType::String)
    {
        ElemSize = sizeof(char *);
        AttrString = (char *)Data;
        DataAddress = (const char *)&AttrString;
    }
    if (ElemCount == (size_t)(-1))
    {
        // simple field, only simple attribute name and value
        char *SstName =
            BuildVarName(Name, ShapeID::GlobalValue, (int)Type, ElemSize);
        AddField(&Info.AttributeFields, &Info.AttributeFieldCount, SstName,
                 Type, ElemSize);
        free(SstName);
        RecalcAttributeStorageSize();
        int DataOffset =
            Info.AttributeFields[Info.AttributeFieldCount - 1].field_offset;
        memcpy((char *)(Info.AttributeData) + DataOffset, DataAddress,
               ElemSize);
    }
    else
    {
        // Array field.  To attribute data add dimension field and dynamic array
        // field
        char *ArrayName = BuildVarName(Name, ShapeID::GlobalArray, 0,
                                       0); // size and type in full field spec
        char *ElemCountName = ConcatName(ArrayName, "ElemCount");
        AddField(&Info.AttributeFields, &Info.AttributeFieldCount,
                 ElemCountName, DataType::Int64, sizeof(int64_t));
        int CountOffset =
            Info.AttributeFields[Info.AttributeFieldCount - 1].field_offset;
        AddVarArrayField(&Info.AttributeFields, &Info.AttributeFieldCount,
                         ArrayName, Type, ElemSize, ElemCountName);
        int DataOffset =
            Info.AttributeFields[Info.AttributeFieldCount - 1].field_offset;
        free(ElemCountName);
        free(ArrayName);

        RecalcAttributeStorageSize();

        memcpy((char *)(Info.AttributeData) + CountOffset, &ElemCount,
               sizeof(size_t));
        memcpy((char *)(Info.AttributeData) + DataOffset, &Data,
               sizeof(void *));
    }
}

void BP5Serializer::InitStep(BufferV *DataBuffer)
{
    if (CurDataBuffer != NULL)
    {
        helper::Throw<std::logic_error>("Toolkit", "format::BP5Serializer",
                                        "InitStep", "without prior Close");
    }
    CurDataBuffer = DataBuffer;
    m_PriorDataBufferSizeTotal = 0;
}

BufferV *BP5Serializer::ReinitStepData(BufferV *DataBuffer,
                                       bool forceCopyDeferred)
{
    if (CurDataBuffer == NULL)
    {
        helper::Throw<std::logic_error>("Toolkit", "format::BP5Serializer",
                                        "ReinitStepData", "without prior Init");
    }
    //  Dump data for externs into iovec
    DumpDeferredBlocks(forceCopyDeferred);

    m_PriorDataBufferSizeTotal += CurDataBuffer->AddToVec(
        0, NULL, m_BufferBlockSize, true); //  output block size aligned

    BufferV *tmp = CurDataBuffer;
    CurDataBuffer = DataBuffer;
    return tmp;
}

BP5Serializer::TimestepInfo BP5Serializer::CloseTimestep(int timestep,
                                                         bool forceCopyDeferred)
{
    std::vector<MetaMetaInfoBlock> Formats;
    if (!Info.MetaFormat && Info.MetaFieldCount)
    {
        MetaMetaInfoBlock Block;
        FMStructDescRec struct_list[20] = {
            {NULL, NULL, 0, NULL},
            {"complex4", fcomplex_field_list, sizeof(fcomplex_struct), NULL},
            {"complex8", dcomplex_field_list, sizeof(dcomplex_struct), NULL},
            {"MetaArray", MetaArrayRecListPtr, sizeof(MetaArrayRec), NULL},
            {"MetaArrayOp", MetaArrayRecOperatorListPtr,
             sizeof(MetaArrayRecOperator), NULL},
            {"MetaArrayMM1", MetaArrayRecMM1ListPtr, sizeof(MetaArrayRecMM),
             NULL},
            {"MetaArrayOpMM1", MetaArrayRecOperatorMM1ListPtr,
             sizeof(MetaArrayRecOperatorMM), NULL},
            {"MetaArrayMM2", MetaArrayRecMM2ListPtr, sizeof(MetaArrayRecMM),
             NULL},
            {"MetaArrayOpMM2", MetaArrayRecOperatorMM2ListPtr,
             sizeof(MetaArrayRecOperatorMM), NULL},
            {"MetaArrayMM4", MetaArrayRecMM4ListPtr, sizeof(MetaArrayRecMM),
             NULL},
            {"MetaArrayOpMM4", MetaArrayRecOperatorMM4ListPtr,
             sizeof(MetaArrayRecOperatorMM), NULL},
            {"MetaArrayMM8", MetaArrayRecMM8ListPtr, sizeof(MetaArrayRecMM),
             NULL},
            {"MetaArrayOpMM8", MetaArrayRecOperatorMM8ListPtr,
             sizeof(MetaArrayRecOperatorMM), NULL},
            {"MetaArrayMM16", MetaArrayRecMM16ListPtr, sizeof(MetaArrayRecMM),
             NULL},
            {"MetaArrayOpMM16", MetaArrayRecOperatorMM16ListPtr,
             sizeof(MetaArrayRecOperatorMM), NULL},
            {NULL, NULL, 0, NULL}};
        struct_list[0].format_name = "MetaData";
        struct_list[0].field_list = Info.MetaFields;
        struct_list[0].struct_size =
            FMstruct_size_field_list(Info.MetaFields, sizeof(char *));

        FMFormat Format =
            register_data_format(Info.LocalFMContext, &struct_list[0]);

        Info.MetaFormat = Format;
        int size;
        Block.MetaMetaInfo = get_server_rep_FMformat(Format, &size);
        Block.MetaMetaInfoLen = size;
        Block.MetaMetaID = get_server_ID_FMformat(Format, &size);
        Block.MetaMetaIDLen = size;
        Formats.push_back(Block);
    }
    if (NewAttribute && Info.AttributeFields)
    {
        MetaMetaInfoBlock Block;
        FMStructDescRec struct_list[4] = {
            {NULL, NULL, 0, NULL},
            {"complex4", fcomplex_field_list, sizeof(fcomplex_struct), NULL},
            {"complex8", dcomplex_field_list, sizeof(dcomplex_struct), NULL},
            {NULL, NULL, 0, NULL}};
        struct_list[0].format_name = "Attributes";
        struct_list[0].field_list = Info.AttributeFields;
        struct_list[0].struct_size =
            FMstruct_size_field_list(Info.AttributeFields, sizeof(char *));

        FMFormat Format =
            register_data_format(Info.LocalFMContext, &struct_list[0]);
        Info.AttributeFormat = Format;
        int size;
        Block.MetaMetaInfo = get_server_rep_FMformat(Format, &size);
        Block.MetaMetaInfoLen = size;
        Block.MetaMetaID = get_server_ID_FMformat(Format, &size);
        Block.MetaMetaIDLen = size;
        Formats.push_back(Block);
    }
    // Encode Metadata and Data to create contiguous data blocks
    FFSBuffer MetaEncodeBuffer = create_FFSBuffer();
    FFSBuffer AttributeEncodeBuffer = NULL;
    int MetaDataSize = 0;
    int AttributeSize = 0;
    struct BP5MetadataInfoStruct *MBase =
        (struct BP5MetadataInfoStruct *)MetadataBuf;

    if (CurDataBuffer == NULL)
    {
        helper::Throw<std::logic_error>("Toolkit", "format::BP5Serializer",
                                        "CloseTimestep", "without prior Init");
    }

    //  Dump data for externs into iovec
    DumpDeferredBlocks(forceCopyDeferred);

    MBase->DataBlockSize = CurDataBuffer->AddToVec(
        0, NULL, m_BufferBlockSize, true); //  output block size aligned

    MBase->DataBlockSize += m_PriorDataBufferSizeTotal;

    void *MetaDataBlock = FFSencode(MetaEncodeBuffer, Info.MetaFormat,
                                    MetadataBuf, &MetaDataSize);
    BufferFFS *Metadata =
        new BufferFFS(MetaEncodeBuffer, MetaDataBlock, MetaDataSize);

    BufferFFS *AttrData = NULL;
    if (NewAttribute && Info.AttributeFields)
    {
        AttributeEncodeBuffer = create_FFSBuffer();
        void *AttributeBlock =
            FFSencode(AttributeEncodeBuffer, Info.AttributeFormat,
                      Info.AttributeData, &AttributeSize);
        AttrData =
            new BufferFFS(AttributeEncodeBuffer, AttributeBlock, AttributeSize);
    }

    // FMdump_encoded_data(Info.MetaFormat, MetaDataBlock, 1024000);
    /* free all those copied dimensions, etc */
    MBase = (struct BP5MetadataInfoStruct *)Metadata;
    size_t *tmp = MBase->BitField;
    /*
     * BitField value is saved away from FMfree_var_rec_elements() so that it
     * isn't unnecessarily free'd.
     */
    MBase->BitField = NULL;
    if (Info.MetaFormat)
        FMfree_var_rec_elements(Info.MetaFormat, MetadataBuf);
    if (MetadataBuf && MetadataSize)
        memset(MetadataBuf, 0, MetadataSize);
    MBase->BitField = tmp;
    NewAttribute = false;

    struct TimestepInfo Ret
    {
        Formats, Metadata, AttrData, CurDataBuffer
    };
    CurDataBuffer = NULL;
    if (Info.AttributeFields)
        free_FMfield_list(Info.AttributeFields);
    Info.AttributeFields = NULL;
    Info.AttributeFieldCount = 0;
    if (Info.AttributeData)
        free(Info.AttributeData);
    Info.AttributeData = NULL;
    Info.AttributeSize = 0;
    return Ret;
}

std::vector<char> BP5Serializer::CopyMetadataToContiguous(
    const std::vector<BP5Base::MetaMetaInfoBlock> NewMetaMetaBlocks,
    const std::vector<core::iovec> &MetaEncodeBuffers,
    const std::vector<core::iovec> &AttributeEncodeBuffers,
    const std::vector<uint64_t> &DataSizes,
    const std::vector<uint64_t> &WriterDataPositions) const
{
    std::vector<char> Ret;
    uint64_t RetSize = 0;
    size_t Position = 0;
    const uint64_t NMMBCount = NewMetaMetaBlocks.size();
    const uint64_t MBCount = MetaEncodeBuffers.size();
    const uint64_t ABCount = AttributeEncodeBuffers.size();
    const uint64_t DSCount = DataSizes.size();
    const uint64_t WDPCount = WriterDataPositions.size();

    // count sizes
    RetSize += sizeof(NMMBCount); // NMMB count
    for (auto &n : NewMetaMetaBlocks)
    {
        RetSize += 2 * sizeof(RetSize); // sizes
        RetSize += n.MetaMetaInfoLen + n.MetaMetaIDLen;
    }
    RetSize += sizeof(MBCount); // Number of var blocks
    for (auto &m : MetaEncodeBuffers)
    {
        RetSize += sizeof(uint64_t); // MencodeLen
        size_t AlignedSize = ((m.iov_len + 7) & ~0x7);
        RetSize += AlignedSize;
    }
    RetSize += sizeof(ABCount); // Number of attr blocks
    for (auto &a : AttributeEncodeBuffers)
    {
        RetSize += sizeof(uint64_t); // AttrEncodeLen
        size_t AlignedSize = ((a.iov_len + 7) & ~0x7);
        RetSize += AlignedSize;
    }
    RetSize += sizeof(DSCount);
    RetSize += DataSizes.size() * sizeof(uint64_t);
    RetSize += sizeof(WDPCount);
    RetSize += WriterDataPositions.size() * sizeof(uint64_t);
    Ret.resize(RetSize);

    // copy
    helper::CopyToBuffer(Ret, Position, &NMMBCount);
    for (auto &n : NewMetaMetaBlocks)
    {
        uint64_t IDLen = n.MetaMetaIDLen;
        uint64_t InfoLen = n.MetaMetaInfoLen;
        helper::CopyToBuffer(Ret, Position, &IDLen);
        helper::CopyToBuffer(Ret, Position, &InfoLen);
        helper::CopyToBuffer(Ret, Position, n.MetaMetaID, IDLen);
        helper::CopyToBuffer(Ret, Position, n.MetaMetaInfo, InfoLen);
    }

    helper::CopyToBuffer(Ret, Position, &MBCount);
    for (auto &m : MetaEncodeBuffers)
    {
        size_t AlignedSize = ((m.iov_len + 7) & ~0x7);
        helper::CopyToBuffer(Ret, Position, &AlignedSize);
        helper::CopyToBuffer(Ret, Position, (const char *)m.iov_base,
                             m.iov_len);
        if (m.iov_len != AlignedSize)
        {
            uint64_t zero = 0;
            helper::CopyToBuffer(Ret, Position, (char *)&zero,
                                 AlignedSize - m.iov_len);
        }
    }

    helper::CopyToBuffer(Ret, Position, &ABCount);
    for (auto &a : AttributeEncodeBuffers)
    {
        if (a.iov_base)
        {
            size_t AlignedSize = ((a.iov_len + 7) & ~0x7);
            helper::CopyToBuffer(Ret, Position, &AlignedSize);
            helper::CopyToBuffer(Ret, Position, (const char *)a.iov_base,
                                 a.iov_len);
            if (a.iov_len != AlignedSize)
            {
                uint64_t zero = 0;
                helper::CopyToBuffer(Ret, Position, (char *)&zero,
                                     AlignedSize - a.iov_len);
            }
        }
        else
        {
            size_t ZeroSize = 0;
            helper::CopyToBuffer(Ret, Position, &ZeroSize);
        }
    }

    helper::CopyToBuffer(Ret, Position, &DSCount);
    helper::CopyToBuffer(Ret, Position, DataSizes.data(), DSCount);
    helper::CopyToBuffer(Ret, Position, &WDPCount);
    helper::CopyToBuffer(Ret, Position, WriterDataPositions.data(), WDPCount);
    return Ret;
}

std::vector<core::iovec> BP5Serializer::BreakoutContiguousMetadata(
    std::vector<char> &Aggregate, const std::vector<size_t> Counts,
    std::vector<MetaMetaInfoBlock> &UniqueMetaMetaBlocks,
    std::vector<core::iovec> &AttributeBlocks, std::vector<uint64_t> &DataSizes,
    std::vector<uint64_t> &WriterDataPositions) const
{
    size_t Position = 0;
    std::vector<core::iovec> MetadataBlocks;
    // MetadataBlocks.reserve(Counts.size());
    // DataSizes.resize(Counts.size());
    for (size_t Rank = 0; Rank < Counts.size(); Rank++)
    {
        uint64_t NMMBCount, MBCount, ABCount, DSCount, WDPCount;
        helper::CopyFromBuffer(Aggregate, Position, &NMMBCount);
        for (uint64_t i = 0; i < NMMBCount; i++)
        {
            uint64_t IDLen;
            uint64_t InfoLen;
            helper::CopyFromBuffer(Aggregate, Position, &IDLen);
            helper::CopyFromBuffer(Aggregate, Position, &InfoLen);
            uint64_t IDPosition = Position;
            uint64_t InfoPosition = Position + IDLen;
            Position = InfoPosition + InfoLen;
            bool Found = 0;
            for (auto &o : UniqueMetaMetaBlocks)
            {
                if (o.MetaMetaIDLen != IDLen)
                    continue;
                if (std::memcmp(o.MetaMetaID, Aggregate.data() + IDPosition,
                                IDLen) == 0)
                    Found = true;
            }
            if (!Found)
            {
                MetaMetaInfoBlock New = {Aggregate.data() + InfoPosition,
                                         InfoLen, Aggregate.data() + IDPosition,
                                         IDLen};
                UniqueMetaMetaBlocks.push_back(New);
            }
        }
        helper::CopyFromBuffer(Aggregate, Position, &MBCount);
        for (uint64_t i = 0; i < MBCount; ++i)
        {
            uint64_t MEBSize;
            helper::CopyFromBuffer(Aggregate, Position, &MEBSize);
            MetadataBlocks.push_back({Aggregate.data() + Position, MEBSize});
            Position += MEBSize;
        }
        helper::CopyFromBuffer(Aggregate, Position, &ABCount);
        for (uint64_t i = 0; i < ABCount; ++i)
        {
            uint64_t AEBSize;
            helper::CopyFromBuffer(Aggregate, Position, &AEBSize);
            AttributeBlocks.push_back({Aggregate.data() + Position, AEBSize});
            Position += AEBSize;
        }
        uint64_t element;
        helper::CopyFromBuffer(Aggregate, Position, &DSCount);
        for (uint64_t i = 0; i < DSCount; ++i)
        {
            helper::CopyFromBuffer(Aggregate, Position, &element);
            DataSizes.push_back(element);
        }
        helper::CopyFromBuffer(Aggregate, Position, &WDPCount);
        for (uint64_t i = 0; i < WDPCount; ++i)
        {
            helper::CopyFromBuffer(Aggregate, Position, &element);
            WriterDataPositions.push_back(element);
        }
    }
    return MetadataBlocks;
}

void *BP5Serializer::GetPtr(int bufferIdx, size_t posInBuffer)
{
    return CurDataBuffer->GetPtr(bufferIdx, posInBuffer);
}

size_t BP5Serializer::DebugGetDataBufferSize() const
{
    if (CurDataBuffer == NULL)
        return 0;
    return CurDataBuffer->Size();
}

} // end namespace format
} // end namespace adios2
