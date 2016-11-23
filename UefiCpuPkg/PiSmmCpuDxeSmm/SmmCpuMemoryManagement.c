/** @file

Copyright (c) 2016, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "PiSmmCpuDxeSmm.h"

#define NEXT_MEMORY_DESCRIPTOR(MemoryDescriptor, Size) \
  ((EFI_MEMORY_DESCRIPTOR *)((UINT8 *)(MemoryDescriptor) + (Size)))

PAGE_ATTRIBUTE_TABLE mPageAttributeTable[] = {
  {Page4K,  SIZE_4KB, PAGING_4K_ADDRESS_MASK_64},
  {Page2M,  SIZE_2MB, PAGING_2M_ADDRESS_MASK_64},
  {Page1G,  SIZE_1GB, PAGING_1G_ADDRESS_MASK_64},
};

/**
  Return page table base.

  @return page table base.
**/
UINTN
GetPageTableBase (
  VOID
  )
{
  return (AsmReadCr3 () & PAGING_4K_ADDRESS_MASK_64);
}

/**
  Return length according to page attributes.

  @param[in]  PageAttributes   The page attribute of the page entry.

  @return The length of page entry.
**/
UINTN
PageAttributeToLength (
  IN PAGE_ATTRIBUTE  PageAttribute
  )
{
  UINTN  Index;
  for (Index = 0; Index < sizeof(mPageAttributeTable)/sizeof(mPageAttributeTable[0]); Index++) {
    if (PageAttribute == mPageAttributeTable[Index].Attribute) {
      return (UINTN)mPageAttributeTable[Index].Length;
    }
  }
  return 0;
}

/**
  Return address mask according to page attributes.

  @param[in]  PageAttributes   The page attribute of the page entry.

  @return The address mask of page entry.
**/
UINTN
PageAttributeToMask (
  IN PAGE_ATTRIBUTE  PageAttribute
  )
{
  UINTN  Index;
  for (Index = 0; Index < sizeof(mPageAttributeTable)/sizeof(mPageAttributeTable[0]); Index++) {
    if (PageAttribute == mPageAttributeTable[Index].Attribute) {
      return (UINTN)mPageAttributeTable[Index].AddressMask;
    }
  }
  return 0;
}

/**
  Return page table entry to match the address.

  @param[in]   Address          The address to be checked.
  @param[out]  PageAttributes   The page attribute of the page entry.

  @return The page entry.
**/
VOID *
GetPageTableEntry (
  IN  PHYSICAL_ADDRESS                  Address,
  OUT PAGE_ATTRIBUTE                    *PageAttribute
  )
{
  UINTN                 Index1;
  UINTN                 Index2;
  UINTN                 Index3;
  UINTN                 Index4;
  UINT64                *L1PageTable;
  UINT64                *L2PageTable;
  UINT64                *L3PageTable;
  UINT64                *L4PageTable;

  Index4 = ((UINTN)RShiftU64 (Address, 39)) & PAGING_PAE_INDEX_MASK;
  Index3 = ((UINTN)Address >> 30) & PAGING_PAE_INDEX_MASK;
  Index2 = ((UINTN)Address >> 21) & PAGING_PAE_INDEX_MASK;
  Index1 = ((UINTN)Address >> 12) & PAGING_PAE_INDEX_MASK;

  if (sizeof(UINTN) == sizeof(UINT64)) {
    L4PageTable = (UINT64 *)GetPageTableBase ();
    if (L4PageTable[Index4] == 0) {
      *PageAttribute = PageNone;
      return NULL;
    }

    L3PageTable = (UINT64 *)(UINTN)(L4PageTable[Index4] & PAGING_4K_ADDRESS_MASK_64);
  } else {
    L3PageTable = (UINT64 *)GetPageTableBase ();
  }
  if (L3PageTable[Index3] == 0) {
    *PageAttribute = PageNone;
    return NULL;
  }
  if ((L3PageTable[Index3] & IA32_PG_PS) != 0) {
    // 1G
    *PageAttribute = Page1G;
    return &L3PageTable[Index3];
  }

  L2PageTable = (UINT64 *)(UINTN)(L3PageTable[Index3] & PAGING_4K_ADDRESS_MASK_64);
  if (L2PageTable[Index2] == 0) {
    *PageAttribute = PageNone;
    return NULL;
  }
  if ((L2PageTable[Index2] & IA32_PG_PS) != 0) {
    // 2M
    *PageAttribute = Page2M;
    return &L2PageTable[Index2];
  }

  // 4k
  L1PageTable = (UINT64 *)(UINTN)(L2PageTable[Index2] & PAGING_4K_ADDRESS_MASK_64);
  if ((L1PageTable[Index1] == 0) && (Address != 0)) {
    *PageAttribute = PageNone;
    return NULL;
  }
  *PageAttribute = Page4K;
  return &L1PageTable[Index1];
}

/**
  Return memory attributes of page entry.

  @param[in]  PageEntry        The page entry.

  @return Memory attributes of page entry.
**/
UINT64
GetAttributesFromPageEntry (
  IN  UINT64                            *PageEntry
  )
{
  UINT64  Attributes;
  Attributes = 0;
  if ((*PageEntry & IA32_PG_P) == 0) {
    Attributes |= EFI_MEMORY_RP;
  }
  if ((*PageEntry & IA32_PG_RW) == 0) {
    Attributes |= EFI_MEMORY_RO;
  }
  if ((*PageEntry & IA32_PG_NX) != 0) {
    Attributes |= EFI_MEMORY_XP;
  }
  return Attributes;
}

/**
  Modify memory attributes of page entry.

  @param[in]   PageEntry        The page entry.
  @param[in]   Attributes       The bit mask of attributes to modify for the memory region.
  @param[in]   IsSet            TRUE means to set attributes. FALSE means to clear attributes.
  @param[out]  IsModified       TRUE means page table modified. FALSE means page table not modified.
**/
VOID
ConvertPageEntryAttribute (
  IN  UINT64                            *PageEntry,
  IN  UINT64                            Attributes,
  IN  BOOLEAN                           IsSet,
  OUT BOOLEAN                           *IsModified
  )
{
  UINT64  CurrentPageEntry;
  UINT64  NewPageEntry;

  CurrentPageEntry = *PageEntry;
  NewPageEntry = CurrentPageEntry;
  if ((Attributes & EFI_MEMORY_RP) != 0) {
    if (IsSet) {
      NewPageEntry &= ~(UINT64)IA32_PG_P;
    } else {
      NewPageEntry |= IA32_PG_P;
    }
  }
  if ((Attributes & EFI_MEMORY_RO) != 0) {
    if (IsSet) {
      NewPageEntry &= ~(UINT64)IA32_PG_RW;
    } else {
      NewPageEntry |= IA32_PG_RW;
    }
  }
  if ((Attributes & EFI_MEMORY_XP) != 0) {
    if (mXdSupported) {
      if (IsSet) {
        NewPageEntry |= IA32_PG_NX;
      } else {
        NewPageEntry &= ~IA32_PG_NX;
      }
    }
  }
  *PageEntry = NewPageEntry;
  if (CurrentPageEntry != NewPageEntry) {
    *IsModified = TRUE;
    DEBUG ((DEBUG_VERBOSE, "ConvertPageEntryAttribute 0x%lx", CurrentPageEntry));
    DEBUG ((DEBUG_VERBOSE, "->0x%lx\n", NewPageEntry));
  } else {
    *IsModified = FALSE;
  }
}

/**
  This function returns if there is need to split page entry.

  @param[in]  BaseAddress      The base address to be checked.
  @param[in]  Length           The length to be checked.
  @param[in]  PageEntry        The page entry to be checked.
  @param[in]  PageAttribute    The page attribute of the page entry.

  @retval SplitAttributes on if there is need to split page entry.
**/
PAGE_ATTRIBUTE
NeedSplitPage (
  IN  PHYSICAL_ADDRESS                  BaseAddress,
  IN  UINT64                            Length,
  IN  UINT64                            *PageEntry,
  IN  PAGE_ATTRIBUTE                    PageAttribute
  )
{
  UINT64                PageEntryLength;

  PageEntryLength = PageAttributeToLength (PageAttribute);

  if (((BaseAddress & (PageEntryLength - 1)) == 0) && (Length >= PageEntryLength)) {
    return PageNone;
  }

  if (((BaseAddress & PAGING_2M_MASK) != 0) || (Length < SIZE_2MB)) {
    return Page4K;
  }

  return Page2M;
}

/**
  This function splits one page entry to small page entries.

  @param[in]  PageEntry        The page entry to be splitted.
  @param[in]  PageAttribute    The page attribute of the page entry.
  @param[in]  SplitAttribute   How to split the page entry.

  @retval RETURN_SUCCESS            The page entry is splitted.
  @retval RETURN_UNSUPPORTED        The page entry does not support to be splitted.
  @retval RETURN_OUT_OF_RESOURCES   No resource to split page entry.
**/
RETURN_STATUS
SplitPage (
  IN  UINT64                            *PageEntry,
  IN  PAGE_ATTRIBUTE                    PageAttribute,
  IN  PAGE_ATTRIBUTE                    SplitAttribute
  )
{
  UINT64   BaseAddress;
  UINT64   *NewPageEntry;
  UINTN    Index;

  ASSERT (PageAttribute == Page2M || PageAttribute == Page1G);

  if (PageAttribute == Page2M) {
    //
    // Split 2M to 4K
    //
    ASSERT (SplitAttribute == Page4K);
    if (SplitAttribute == Page4K) {
      NewPageEntry = AllocatePageTableMemory (1);
      DEBUG ((DEBUG_VERBOSE, "Split - 0x%x\n", NewPageEntry));
      if (NewPageEntry == NULL) {
        return RETURN_OUT_OF_RESOURCES;
      }
      BaseAddress = *PageEntry & PAGING_2M_ADDRESS_MASK_64;
      for (Index = 0; Index < SIZE_4KB / sizeof(UINT64); Index++) {
        NewPageEntry[Index] = BaseAddress + SIZE_4KB * Index + ((*PageEntry) & PAGE_PROGATE_BITS);
      }
      (*PageEntry) = (UINT64)(UINTN)NewPageEntry + PAGE_ATTRIBUTE_BITS;
      return RETURN_SUCCESS;
    } else {
      return RETURN_UNSUPPORTED;
    }
  } else if (PageAttribute == Page1G) {
    //
    // Split 1G to 2M
    // No need support 1G->4K directly, we should use 1G->2M, then 2M->4K to get more compact page table.
    //
    ASSERT (SplitAttribute == Page2M || SplitAttribute == Page4K);
    if ((SplitAttribute == Page2M || SplitAttribute == Page4K)) {
      NewPageEntry = AllocatePageTableMemory (1);
      DEBUG ((DEBUG_VERBOSE, "Split - 0x%x\n", NewPageEntry));
      if (NewPageEntry == NULL) {
        return RETURN_OUT_OF_RESOURCES;
      }
      BaseAddress = *PageEntry & PAGING_1G_ADDRESS_MASK_64;
      for (Index = 0; Index < SIZE_4KB / sizeof(UINT64); Index++) {
        NewPageEntry[Index] = BaseAddress + SIZE_2MB * Index + IA32_PG_PS + ((*PageEntry) & PAGE_PROGATE_BITS);
      }
      (*PageEntry) = (UINT64)(UINTN)NewPageEntry + PAGE_ATTRIBUTE_BITS;
      return RETURN_SUCCESS;
    } else {
      return RETURN_UNSUPPORTED;
    }
  } else {
    return RETURN_UNSUPPORTED;
  }
}

/**
  This function modifies the page attributes for the memory region specified by BaseAddress and
  Length from their current attributes to the attributes specified by Attributes.

  Caller should make sure BaseAddress and Length is at page boundary.

  @param[in]   BaseAddress      The physical address that is the start address of a memory region.
  @param[in]   Length           The size in bytes of the memory region.
  @param[in]   Attributes       The bit mask of attributes to modify for the memory region.
  @param[in]   IsSet            TRUE means to set attributes. FALSE means to clear attributes.
  @param[out]  IsSplitted       TRUE means page table splitted. FALSE means page table not splitted.
  @param[out]  IsModified       TRUE means page table modified. FALSE means page table not modified.

  @retval RETURN_SUCCESS           The attributes were modified for the memory region.
  @retval RETURN_ACCESS_DENIED     The attributes for the memory resource range specified by
                                   BaseAddress and Length cannot be modified.
  @retval RETURN_INVALID_PARAMETER Length is zero.
                                   Attributes specified an illegal combination of attributes that
                                   cannot be set together.
  @retval RETURN_OUT_OF_RESOURCES  There are not enough system resources to modify the attributes of
                                   the memory resource range.
  @retval RETURN_UNSUPPORTED       The processor does not support one or more bytes of the memory
                                   resource range specified by BaseAddress and Length.
                                   The bit mask of attributes is not support for the memory resource
                                   range specified by BaseAddress and Length.
**/
RETURN_STATUS
EFIAPI
ConvertMemoryPageAttributes (
  IN  PHYSICAL_ADDRESS                  BaseAddress,
  IN  UINT64                            Length,
  IN  UINT64                            Attributes,
  IN  BOOLEAN                           IsSet,
  OUT BOOLEAN                           *IsSplitted,  OPTIONAL
  OUT BOOLEAN                           *IsModified   OPTIONAL
  )
{
  UINT64                            *PageEntry;
  PAGE_ATTRIBUTE                    PageAttribute;
  UINTN                             PageEntryLength;
  PAGE_ATTRIBUTE                    SplitAttribute;
  RETURN_STATUS                     Status;
  BOOLEAN                           IsEntryModified;

  ASSERT (Attributes != 0);
  ASSERT ((Attributes & ~(EFI_MEMORY_RP | EFI_MEMORY_RO | EFI_MEMORY_XP)) == 0);

  ASSERT ((BaseAddress & (SIZE_4KB - 1)) == 0);
  ASSERT ((Length & (SIZE_4KB - 1)) == 0);

  if (Length == 0) {
    return RETURN_INVALID_PARAMETER;
  }

//  DEBUG ((DEBUG_ERROR, "ConvertMemoryPageAttributes(%x) - %016lx, %016lx, %02lx\n", IsSet, BaseAddress, Length, Attributes));

  if (IsSplitted != NULL) {
    *IsSplitted = FALSE;
  }
  if (IsModified != NULL) {
    *IsModified = FALSE;
  }

  //
  // Below logic is to check 2M/4K page to make sure we donot waist memory.
  //
  while (Length != 0) {
    PageEntry = GetPageTableEntry (BaseAddress, &PageAttribute);
    if (PageEntry == NULL) {
      return RETURN_UNSUPPORTED;
    }
    PageEntryLength = PageAttributeToLength (PageAttribute);
    SplitAttribute = NeedSplitPage (BaseAddress, Length, PageEntry, PageAttribute);
    if (SplitAttribute == PageNone) {
      ConvertPageEntryAttribute (PageEntry, Attributes, IsSet, &IsEntryModified);
      if (IsEntryModified) {
        if (IsModified != NULL) {
          *IsModified = TRUE;
        }
      }
      //
      // Convert success, move to next
      //
      BaseAddress += PageEntryLength;
      Length -= PageEntryLength;
    } else {
      Status = SplitPage (PageEntry, PageAttribute, SplitAttribute);
      if (RETURN_ERROR (Status)) {
        return RETURN_UNSUPPORTED;
      }
      if (IsSplitted != NULL) {
        *IsSplitted = TRUE;
      }
      if (IsModified != NULL) {
        *IsModified = TRUE;
      }
      //
      // Just split current page
      // Convert success in next around
      //
    }
  }

  return RETURN_SUCCESS;
}

/**
  FlushTlb on current processor.

  @param[in,out] Buffer  Pointer to private data buffer.
**/
VOID
EFIAPI
FlushTlbOnCurrentProcessor (
  IN OUT VOID  *Buffer
  )
{
  CpuFlushTlb ();
}

/**
  FlushTlb for all processors.
**/
VOID
FlushTlbForAll (
  VOID
  )
{
  UINTN       Index;

  FlushTlbOnCurrentProcessor (NULL);

  for (Index = 0; Index < gSmst->NumberOfCpus; Index++) {
    if (Index != gSmst->CurrentlyExecutingCpu) {
      // Force to start up AP in blocking mode,
      SmmBlockingStartupThisAp (FlushTlbOnCurrentProcessor, Index, NULL);
      // Do not check return status, because AP might not be present in some corner cases.
    }
  }
}

/**
  This function sets the attributes for the memory region specified by BaseAddress and
  Length from their current attributes to the attributes specified by Attributes.

  @param[in]   BaseAddress      The physical address that is the start address of a memory region.
  @param[in]   Length           The size in bytes of the memory region.
  @param[in]   Attributes       The bit mask of attributes to set for the memory region.
  @param[out]  IsSplitted       TRUE means page table splitted. FALSE means page table not splitted.

  @retval EFI_SUCCESS           The attributes were set for the memory region.
  @retval EFI_ACCESS_DENIED     The attributes for the memory resource range specified by
                                BaseAddress and Length cannot be modified.
  @retval EFI_INVALID_PARAMETER Length is zero.
                                Attributes specified an illegal combination of attributes that
                                cannot be set together.
  @retval EFI_OUT_OF_RESOURCES  There are not enough system resources to modify the attributes of
                                the memory resource range.
  @retval EFI_UNSUPPORTED       The processor does not support one or more bytes of the memory
                                resource range specified by BaseAddress and Length.
                                The bit mask of attributes is not support for the memory resource
                                range specified by BaseAddress and Length.

**/
EFI_STATUS
EFIAPI
SmmSetMemoryAttributesEx (
  IN  EFI_PHYSICAL_ADDRESS                       BaseAddress,
  IN  UINT64                                     Length,
  IN  UINT64                                     Attributes,
  OUT BOOLEAN                                    *IsSplitted  OPTIONAL
  )
{
  EFI_STATUS  Status;
  BOOLEAN     IsModified;

  Status = ConvertMemoryPageAttributes (BaseAddress, Length, Attributes, TRUE, IsSplitted, &IsModified);
  if (!EFI_ERROR(Status)) {
    if (IsModified) {
      //
      // Flush TLB as last step
      //
      FlushTlbForAll();
    }
  }

  return Status;
}

/**
  This function clears the attributes for the memory region specified by BaseAddress and
  Length from their current attributes to the attributes specified by Attributes.

  @param[in]   BaseAddress      The physical address that is the start address of a memory region.
  @param[in]   Length           The size in bytes of the memory region.
  @param[in]   Attributes       The bit mask of attributes to clear for the memory region.
  @param[out]  IsSplitted       TRUE means page table splitted. FALSE means page table not splitted.

  @retval EFI_SUCCESS           The attributes were cleared for the memory region.
  @retval EFI_ACCESS_DENIED     The attributes for the memory resource range specified by
                                BaseAddress and Length cannot be modified.
  @retval EFI_INVALID_PARAMETER Length is zero.
                                Attributes specified an illegal combination of attributes that
                                cannot be set together.
  @retval EFI_OUT_OF_RESOURCES  There are not enough system resources to modify the attributes of
                                the memory resource range.
  @retval EFI_UNSUPPORTED       The processor does not support one or more bytes of the memory
                                resource range specified by BaseAddress and Length.
                                The bit mask of attributes is not support for the memory resource
                                range specified by BaseAddress and Length.

**/
EFI_STATUS
EFIAPI
SmmClearMemoryAttributesEx (
  IN  EFI_PHYSICAL_ADDRESS                       BaseAddress,
  IN  UINT64                                     Length,
  IN  UINT64                                     Attributes,
  OUT BOOLEAN                                    *IsSplitted  OPTIONAL
  )
{
  EFI_STATUS  Status;
  BOOLEAN     IsModified;

  Status = ConvertMemoryPageAttributes (BaseAddress, Length, Attributes, FALSE, IsSplitted, &IsModified);
  if (!EFI_ERROR(Status)) {
    if (IsModified) {
      //
      // Flush TLB as last step
      //
      FlushTlbForAll();
    }
  }

  return Status;
}

/**
  This function sets the attributes for the memory region specified by BaseAddress and
  Length from their current attributes to the attributes specified by Attributes.

  @param[in]  BaseAddress      The physical address that is the start address of a memory region.
  @param[in]  Length           The size in bytes of the memory region.
  @param[in]  Attributes       The bit mask of attributes to set for the memory region.

  @retval EFI_SUCCESS           The attributes were set for the memory region.
  @retval EFI_ACCESS_DENIED     The attributes for the memory resource range specified by
                                BaseAddress and Length cannot be modified.
  @retval EFI_INVALID_PARAMETER Length is zero.
                                Attributes specified an illegal combination of attributes that
                                cannot be set together.
  @retval EFI_OUT_OF_RESOURCES  There are not enough system resources to modify the attributes of
                                the memory resource range.
  @retval EFI_UNSUPPORTED       The processor does not support one or more bytes of the memory
                                resource range specified by BaseAddress and Length.
                                The bit mask of attributes is not support for the memory resource
                                range specified by BaseAddress and Length.

**/
EFI_STATUS
EFIAPI
SmmSetMemoryAttributes (
  IN  EFI_PHYSICAL_ADDRESS                       BaseAddress,
  IN  UINT64                                     Length,
  IN  UINT64                                     Attributes
  )
{
  return SmmSetMemoryAttributesEx (BaseAddress, Length, Attributes, NULL);
}

/**
  This function clears the attributes for the memory region specified by BaseAddress and
  Length from their current attributes to the attributes specified by Attributes.

  @param[in]  BaseAddress      The physical address that is the start address of a memory region.
  @param[in]  Length           The size in bytes of the memory region.
  @param[in]  Attributes       The bit mask of attributes to clear for the memory region.

  @retval EFI_SUCCESS           The attributes were cleared for the memory region.
  @retval EFI_ACCESS_DENIED     The attributes for the memory resource range specified by
                                BaseAddress and Length cannot be modified.
  @retval EFI_INVALID_PARAMETER Length is zero.
                                Attributes specified an illegal combination of attributes that
                                cannot be set together.
  @retval EFI_OUT_OF_RESOURCES  There are not enough system resources to modify the attributes of
                                the memory resource range.
  @retval EFI_UNSUPPORTED       The processor does not support one or more bytes of the memory
                                resource range specified by BaseAddress and Length.
                                The bit mask of attributes is not support for the memory resource
                                range specified by BaseAddress and Length.

**/
EFI_STATUS
EFIAPI
SmmClearMemoryAttributes (
  IN  EFI_PHYSICAL_ADDRESS                       BaseAddress,
  IN  UINT64                                     Length,
  IN  UINT64                                     Attributes
  )
{
  return SmmClearMemoryAttributesEx (BaseAddress, Length, Attributes, NULL);
}



/**
  Retrieves a pointer to the system configuration table from the SMM System Table
  based on a specified GUID.

  @param[in]   TableGuid       The pointer to table's GUID type.
  @param[out]  Table           The pointer to the table associated with TableGuid in the EFI System Table.

  @retval EFI_SUCCESS     A configuration table matching TableGuid was found.
  @retval EFI_NOT_FOUND   A configuration table matching TableGuid could not be found.

**/
EFI_STATUS
EFIAPI
SmmGetSystemConfigurationTable (
  IN  EFI_GUID  *TableGuid,
  OUT VOID      **Table
  )
{
  UINTN             Index;

  ASSERT (TableGuid != NULL);
  ASSERT (Table != NULL);

  *Table = NULL;
  for (Index = 0; Index < gSmst->NumberOfTableEntries; Index++) {
    if (CompareGuid (TableGuid, &(gSmst->SmmConfigurationTable[Index].VendorGuid))) {
      *Table = gSmst->SmmConfigurationTable[Index].VendorTable;
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

/**
  This function sets SMM save state buffer to be RW and XP.
**/
VOID
PatchSmmSaveStateMap (
  VOID
  )
{
  UINTN  Index;
  UINTN  TileCodeSize;
  UINTN  TileDataSize;
  UINTN  TileSize;

  TileCodeSize = GetSmiHandlerSize ();
  TileCodeSize = ALIGN_VALUE(TileCodeSize, SIZE_4KB);
  TileDataSize = (SMRAM_SAVE_STATE_MAP_OFFSET - SMM_PSD_OFFSET) + sizeof (SMRAM_SAVE_STATE_MAP);
  TileDataSize = ALIGN_VALUE(TileDataSize, SIZE_4KB);
  TileSize = TileDataSize + TileCodeSize - 1;
  TileSize = 2 * GetPowerOfTwo32 ((UINT32)TileSize);

  DEBUG ((DEBUG_INFO, "PatchSmmSaveStateMap:\n"));
  for (Index = 0; Index < mMaxNumberOfCpus - 1; Index++) {
    //
    // Code
    //
    SmmSetMemoryAttributes (
      mCpuHotPlugData.SmBase[Index] + SMM_HANDLER_OFFSET,
      TileCodeSize,
      EFI_MEMORY_RO
      );
    SmmClearMemoryAttributes (
      mCpuHotPlugData.SmBase[Index] + SMM_HANDLER_OFFSET,
      TileCodeSize,
      EFI_MEMORY_XP
      );

    //
    // Data
    //
    SmmClearMemoryAttributes (
      mCpuHotPlugData.SmBase[Index] + SMM_HANDLER_OFFSET + TileCodeSize,
      TileSize - TileCodeSize,
      EFI_MEMORY_RO
      );
    SmmSetMemoryAttributes (
      mCpuHotPlugData.SmBase[Index] + SMM_HANDLER_OFFSET + TileCodeSize,
      TileSize - TileCodeSize,
      EFI_MEMORY_XP
      );
  }

  //
  // Code
  //
  SmmSetMemoryAttributes (
    mCpuHotPlugData.SmBase[mMaxNumberOfCpus - 1] + SMM_HANDLER_OFFSET,
    TileCodeSize,
    EFI_MEMORY_RO
    );
  SmmClearMemoryAttributes (
    mCpuHotPlugData.SmBase[mMaxNumberOfCpus - 1] + SMM_HANDLER_OFFSET,
    TileCodeSize,
    EFI_MEMORY_XP
    );

  //
  // Data
  //
  SmmClearMemoryAttributes (
    mCpuHotPlugData.SmBase[mMaxNumberOfCpus - 1] + SMM_HANDLER_OFFSET + TileCodeSize,
    SIZE_32KB - TileCodeSize,
    EFI_MEMORY_RO
    );
  SmmSetMemoryAttributes (
    mCpuHotPlugData.SmBase[mMaxNumberOfCpus - 1] + SMM_HANDLER_OFFSET + TileCodeSize,
    SIZE_32KB - TileCodeSize,
    EFI_MEMORY_XP
    );
}

/**
  This function sets memory attribute according to MemoryAttributesTable.
**/
VOID
SetMemMapAttributes (
  VOID
  )
{
  EFI_MEMORY_DESCRIPTOR                     *MemoryMap;
  EFI_MEMORY_DESCRIPTOR                     *MemoryMapStart;
  UINTN                                     MemoryMapEntryCount;
  UINTN                                     DescriptorSize;
  UINTN                                     Index;
  EDKII_PI_SMM_MEMORY_ATTRIBUTES_TABLE      *MemoryAttributesTable;

  SmmGetSystemConfigurationTable (&gEdkiiPiSmmMemoryAttributesTableGuid, (VOID **)&MemoryAttributesTable);
  if (MemoryAttributesTable == NULL) {
    DEBUG ((DEBUG_INFO, "MemoryAttributesTable - NULL\n"));
    return ;
  }

  DEBUG ((DEBUG_INFO, "MemoryAttributesTable:\n"));
  DEBUG ((DEBUG_INFO, "  Version                   - 0x%08x\n", MemoryAttributesTable->Version));
  DEBUG ((DEBUG_INFO, "  NumberOfEntries           - 0x%08x\n", MemoryAttributesTable->NumberOfEntries));
  DEBUG ((DEBUG_INFO, "  DescriptorSize            - 0x%08x\n", MemoryAttributesTable->DescriptorSize));

  MemoryMapEntryCount = MemoryAttributesTable->NumberOfEntries;
  DescriptorSize = MemoryAttributesTable->DescriptorSize;
  MemoryMapStart = (EFI_MEMORY_DESCRIPTOR *)(MemoryAttributesTable + 1);
  MemoryMap = MemoryMapStart;
  for (Index = 0; Index < MemoryMapEntryCount; Index++) {
    DEBUG ((DEBUG_INFO, "Entry (0x%x)\n", MemoryMap));
    DEBUG ((DEBUG_INFO, "  Type              - 0x%x\n", MemoryMap->Type));
    DEBUG ((DEBUG_INFO, "  PhysicalStart     - 0x%016lx\n", MemoryMap->PhysicalStart));
    DEBUG ((DEBUG_INFO, "  VirtualStart      - 0x%016lx\n", MemoryMap->VirtualStart));
    DEBUG ((DEBUG_INFO, "  NumberOfPages     - 0x%016lx\n", MemoryMap->NumberOfPages));
    DEBUG ((DEBUG_INFO, "  Attribute         - 0x%016lx\n", MemoryMap->Attribute));
    MemoryMap = NEXT_MEMORY_DESCRIPTOR(MemoryMap, DescriptorSize);
  }

  MemoryMap = MemoryMapStart;
  for (Index = 0; Index < MemoryMapEntryCount; Index++) {
    DEBUG ((DEBUG_VERBOSE, "SetAttribute: Memory Entry - 0x%lx, 0x%x\n", MemoryMap->PhysicalStart, MemoryMap->NumberOfPages));
    switch (MemoryMap->Type) {
    case EfiRuntimeServicesCode:
      SmmSetMemoryAttributes (
        MemoryMap->PhysicalStart,
        EFI_PAGES_TO_SIZE((UINTN)MemoryMap->NumberOfPages),
        EFI_MEMORY_RO
        );
      break;
    case EfiRuntimeServicesData:
      SmmSetMemoryAttributes (
        MemoryMap->PhysicalStart,
        EFI_PAGES_TO_SIZE((UINTN)MemoryMap->NumberOfPages),
        EFI_MEMORY_XP
        );
      break;
    default:
      SmmSetMemoryAttributes (
        MemoryMap->PhysicalStart,
        EFI_PAGES_TO_SIZE((UINTN)MemoryMap->NumberOfPages),
        EFI_MEMORY_XP
        );
      break;
    }
    MemoryMap = NEXT_MEMORY_DESCRIPTOR(MemoryMap, DescriptorSize);
  }

  PatchSmmSaveStateMap ();
  PatchGdtIdtMap ();

  return ;
}
