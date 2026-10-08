.macro MAKEPATCH_BYTES PAddress
.if(\PAddress)
        .set PatchAddr, \PAddress
        .long PatchAddr
        .long (9f - 0f)
.endif
.endm

.macro MAKEBRANCH DAddress
.if(\DAddress)
        b \DAddress - ((.-0b)+PatchAddr)
.endif
.endm

.macro MAKEBRANCHL DAddress
.if(\DAddress)
        bl \DAddress - ((.-0b)+PatchAddr)
.endif
.endm

        .globl _start
_start:

.set ldBranchAddr,  0x80061414
.set loaderFuncAddr, 0x801616C4
.set XexLoadImage,    0x8007D7C0

MAKEPATCH_BYTES loaderFuncAddr
0:
loaderStart:
        .set hddPathAddr, (loaderFuncAddr + (hddPath - loaderStart))

        lis     %r11, hddPathAddr@h
        ori     %r3, %r11, hddPathAddr@l
        li      %r4, 8
        li      %r5, 0
        li      %r6, 0
        MAKEBRANCHL XexLoadImage

        li      %r3, 0
        MAKEBRANCH (ldBranchAddr + 4)

hddPath:
        .string "\\Device\\Harddisk0\\Partition1\\PeerPressure\\pploader.xex\0"
        .align 2
9:

MAKEPATCH_BYTES ldBranchAddr
0:
        MAKEBRANCHL loaderFuncAddr
9:

        .long 0xFFFFFFFF
        .end
