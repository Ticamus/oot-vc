MEMORY
{
    text : origin = $ORIGIN
}

SECTIONS
{
    GROUP:
    {
        $SECTIONS
        /* New, non-retail sections for the visual crash debugger (crashScreen.c). Placed
           after .sbss2 so every retail section keeps its exact address; only the (computed,
           non-pinned) stack/arena below shift down to make room. */
        .crashtext ALIGN(0x20):{}
        .crashdata ALIGN(0x20):{}
        .crashbss ALIGN(0x20):{}
        .stack ALIGN(0x100):{}
    } > text

    _stack_end = _f_crashbss + SIZEOF(.crashbss);
    _stack_addr = (_stack_end + $STACKSIZE + 0x7) & ~0x7;
    _db_stack_addr = (_stack_addr + 0x2000);
    _db_stack_end = _stack_addr;
    __ArenaLo = (_db_stack_addr + 0x1f) & ~0x1f;
    __ArenaHi = 0x81700000;
}

FORCEACTIVE
{
    $FORCEACTIVE
}
