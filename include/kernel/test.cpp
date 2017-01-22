#include<stdio.h>
#include<stdlib.h>

#include "iot_memalloc.h"
#include "iot_common.h"

int main() {
	iot_memobject obj;
	mpsc_queue<iot_memobject, iot_memobject, &iot_memobject::next> q;
	printf("Size=%u\n",unsigned(offsetof(iot_memobject, data)));
	printf("QSize=%u\n",sizeof(q));
	obj.parent=(iot_memallocator*)0x1234567812345600ul;
	obj.refcount=1;
	obj.listindex=8;
	printf("ptr=%lx\n",(uintptr_t)(obj.parent));

	return 0;
}