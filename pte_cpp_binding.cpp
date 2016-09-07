#include "pthread.h"

bool operator==(const pte_handle_t& l, const pte_handle_t& r)
{
	return (l.p == r.p && l.x == r.x);
}

bool operator<(const pte_handle_t& l, const pte_handle_t& r)
{
	return (l.p < r.p) || (l.p == r.p && l.x < r.x);
}
