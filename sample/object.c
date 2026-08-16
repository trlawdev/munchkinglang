/* Shared-library helper for sample/reflexpr_.mx FFI demo. */
int add_int(int a, int b)
{
    return a + b;
}

void set_int(int *p)
{
    if (p)
    {
        *p = 42;
    }
}
