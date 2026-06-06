
#include	"averager.h"
#include	<cstring>

	average::average (int16_t s) {
	size	= s;
	vec	= new float [s];
	filp	= 0;
	sum	= 0.0f;
	memset (vec, 0, s * sizeof (float));
}

	average::~average () {
	delete[]	vec;
}

// O(1) running-sum implementation — replaces the original O(n) loop.
float	average::filter (float e) {
	sum   -= vec [filp];
	vec [filp] = e;
	sum   += e;
	filp   = (filp + 1) % size;
	return sum / size;
}

void	average::clear (float c) {
int16_t	i;

	for (i = 0; i < size; i ++)
	   vec [i] = c;
	sum  = c * size;
	filp = 0;
}
