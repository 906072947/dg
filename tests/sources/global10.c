#include <assert.h>

int glob;
int *setglob(void)
{
	glob = 23;
}

int *foo(int *(f)(void))
{
	return f();
}

int main(void)
{
	int *p = foo(setglob);
	assert(*p == 23);
	return 0;
}
