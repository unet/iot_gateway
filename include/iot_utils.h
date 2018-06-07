#ifndef IOT_UTILS_H
#define IOT_UTILS_H

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <atomic>

#ifndef NDEBUG
	#include <execinfo.h>
#endif


#define ECB_NO_LIBM
#include "ecb.h"

#define expect_false(cond) ecb_expect_false (cond)
#define expect_true(cond)  ecb_expect_true  (cond)

#include "uv.h"
#include "iot_error.h"

//Functions to be used during serialization/deserialization to get fixed (little endian) byte order
#if ECB_LITTLE_ENDIAN
//#if ECB_BIG_ENDIAN
#define repack_is_nullop 1
#define repack_uint64(v) (v)
#define repack_int64(v) (v)
#define repack_uint32(v) (v)
#define repack_int32(v) (v)
#define repack_uint16(v) (v)
#define repack_int16(v) (v)
#define repack_float(v) (v)
#define repack_double(v) (v)
#elif ECB_BIG_ENDIAN
//#elif ECB_LITTLE_ENDIAN
#define repack_is_nullop 0
#define repack_uint64(v) ecb_bswap64(v)
#define repack_int64(v) ((int64_t)ecb_bswap64(v))
#define repack_uint32(v) ecb_bswap32(v)
#define repack_int32(v) ((int32_t)ecb_bswap32(v))
#define repack_uint16(v) ecb_bswap16(v)
#define repack_int16(v) ((int16_t)ecb_bswap16(v))
ecb_inline ecb_const float repack_float(float v) {uint32_t u=ecb_bswap32(*(uint32_t*)((char*)&(v)));return *(float*)((char*)&u);}
ecb_inline ecb_const double repack_double(double v) {uint64_t u=ecb_bswap64(*(uint64_t*)((char*)&(v)));return *(double*)((char*)&u);}
#else
#error Byte order not constantly determined (TODO)
#endif

#ifdef _MSC_VER
	#define PACKED( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
#elif defined(__GNUC__)
	#define PACKED( __Declaration__ ) __Declaration__ __attribute__((packed))
#else
	#error Cannot set PACKED for current compiler
#endif
//Find correct event loop for specified thread
//Returns 0 on success and fills loop with correct pointer
//On error returns negative error code:
//IOT_ERROR_INVALID_ARGS - thread is unknown or loop is NULL
uv_loop_t* kapi_get_event_loop(uv_thread_t thread);


#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))


#define IOT_JSONPARSE_UINT(jsonval, typename, varname) { \
	if(typename ## _MAX == UINT32_MAX) {		\
		errno=0;	\
		int64_t i64=json_object_get_int64(jsonval);		\
		if(!errno && i64>=0 && i64<=UINT32_MAX) varname=(typename)i64;	\
	} else if(typename ## _MAX == UINT64_MAX) {							\
		uint64_t u64=iot_strtou64(json_object_get_string(jsonval), NULL, 10);	\
		if(!errno && u64>=0 && (u64<INT64_MAX || json_object_is_type(jsonval, json_type_string))) varname=(typename)(u64);	\
	} else if(typename ## _MAX == UINT16_MAX || typename ## _MAX == UINT8_MAX) {							\
		errno=0;	\
		int i=json_object_get_int(jsonval);		\
		if(!errno && i>=0 && (uint32_t)i <= typename ## _MAX) varname=(typename)i;	\
	} else {	\
		assert(false);	\
	}	\
}

#define IOT_STRPARSE_UINT(str, typename, varname) { \
	if(typename ## _MAX == UINT32_MAX) {		\
		uint32_t u32=iot_strtou32(str, NULL, 10);	\
		if(!errno && u32>=0) varname=(typename)(u32);	\
	} else if(typename ## _MAX == UINT64_MAX) {							\
		uint64_t u64=iot_strtou64(str, NULL, 10);	\
		if(!errno && u64>=0) varname=(typename)(u64);	\
	} else if(typename ## _MAX == UINT16_MAX || typename ## _MAX == UINT8_MAX) {							\
		uint32_t u32=iot_strtou32(str, NULL, 10);	\
		if(!errno && u32>=0 && u32 <= typename ## _MAX) varname=(typename)(u32);	\
	} else {	\
		assert(false);	\
	}	\
}


//on overflow UINT64_MAX is returned and errno is set to ERANGE 
inline uint64_t iot_strtou64(const char* str, char **endptr, int base) {
	errno=0;
#if ULONG_MAX == 0xFFFFFFFFUL
	return strtoull(str, endptr, base);
#else
	return strtoul(str, endptr, base);
#endif
}

//on overflow UINT32_MAX is returned and errno is set to ERANGE 
inline uint32_t iot_strtou32(const char* str, char **endptr, int base) {
	errno=0;
#if ULONG_MAX == 0xFFFFFFFFUL
	//32-bit system
	return strtoul(str, endptr, base);
#else
	//64-bit system
	uint64_t rval=strtoul(str, endptr, base);
	if(errno==ERANGE || rval>UINT32_MAX) {
		errno=ERANGE;
		rval=UINT32_MAX;
	}
	return rval;
#endif
}

//converts integer version into string representation like "VER.PATCHLEVEL[:REVISION]" (revision present when non-zero only)
inline char* iot_version_str(uint32_t ver, char* buf, size_t bufsize) { //maximum result length is 13 plus NUL
	unsigned vers, patch, rev;
	vers=ver>>24;
	patch=(ver>>16)&0xFF;
	rev=ver&0xFFFF;
	if(rev>0) snprintf(buf, bufsize, "%u.%u:%u", vers, patch, rev);
		else snprintf(buf, bufsize, "%u.%u", vers, patch);
	return buf;
}

//convers string version representation like "VER.PATCHLEVEL[:REVISION]" into integer. returns UINT32_MAX on parse error
uint32_t iot_parse_version(const char* s);

void iot_gen_random(char* buf, uint16_t len);

//Uni-linked list without tail (NULL value of next field tells about EOL). headvar is of the same type as itemptr. Empty list has headvar==NULL
//Insert item with address itemptr at head of list. nextfld - name of field inside item struct for linking to next item
#define ULINKLIST_INSERTHEAD(itemptr, headvar, nextfld)	\
	do {												\
		(itemptr)->nextfld = headvar;					\
		headvar = itemptr;								\
	} while(0)

//removing of any element when previous item is tracked. itemptr and previtemptr CANNOT BE same expression as headvar
#define ULINKLIST_REMOVE(itemptr, previtemptr, headvar, nextfld)	\
	do {											\
		if((itemptr)==headvar) {					\
			headvar=(itemptr)->nextfld;				\
		} else {									\
			(previtemptr)->nextfld=(itemptr)->nextfld;	\
		}											\
		(itemptr)->nextfld=NULL;					\
	} while(0)

//removing of any element when previous item is tracked. itemptr and previtemptr CANNOT BE same expression as headvar
#define ULINKLIST_REMOVE_NOCL(itemptr, previtemptr, headvar, nextfld)	\
	do {											\
		if((itemptr)==headvar) {					\
			headvar=(itemptr)->nextfld;				\
		} else {									\
			(previtemptr)->nextfld=(itemptr)->nextfld;	\
		}											\
	} while(0)

//removing of any element when previous item is not tracked. itemptr CANNOT BE same expression as headvar
#define ULINKLIST_REMOVE_NOPREV(itemptr, headvar, nextfld)	\
	do {											\
		if((itemptr)==headvar) {					\
			headvar=(itemptr)->nextfld;				\
			(itemptr)->nextfld=NULL;				\
		} else {									\
			auto t=headvar;							\
			while(t->nextfld) {						\
				if(t->nextfld==(itemptr)) {			\
					t->nextfld=(itemptr)->nextfld;	\
					(itemptr)->nextfld=NULL;		\
					break;							\
				}									\
				t=t->nextfld;						\
			}										\
		}											\
	} while(0)

//version of remove without cleaning next field value. itemptr CANNOT BE same expression as headvar
#define ULINKLIST_REMOVE_NOCL_NOPREV(itemptr, headvar, nextfld)	\
	do {											\
		if((itemptr)==headvar) {					\
			headvar=(itemptr)->nextfld;				\
		} else {									\
			auto t=headvar;							\
			while(t->nextfld) {						\
				if(t->nextfld==(itemptr)) {			\
					t->nextfld=(itemptr)->nextfld;	\
					break;							\
				}									\
				t=t->nextfld;						\
			}										\
		}											\
	} while(0)

//removing of head element. must NOT be used without check that headvar is not NULL
#define ULINKLIST_REMOVEHEAD(headvar, nextfld)		\
	do {											\
		auto tmp=headvar;							\
		headvar=(headvar)->nextfld;					\
		tmp->nextfld=NULL;							\
	} while(0)

//version of head remove without cleaning next field value. must NOT be used without check that headvar is not NULL
#define ULINKLIST_REMOVEHEAD_NOCL(headvar, nextfld)	\
	do {											\
		headvar=(headvar)->nextfld;					\
	} while(0)


//Bi-linked list without tail (NULL value of next field tells about EOL, prev value of first item contains special mark and address of head variable)
//Have important features:
// - head pointer is NULL for empty list
// - prev field is NULL for disconnected items only;
// - can be quickly removed from parent list without necessity to explicitely keep any identification of parent list

//variant without cleaning next and prev pointers to be used just before inserting into another bi-linked list or freeing
#define BILINKLIST_REMOVE_NOCL(itemptr, nextfld, prevfld) \
	do {																									\
		if(!(itemptr)->prevfld) { /*disconnected item*/														\
			assert((itemptr)->nextfld==NULL); /*cannot have next*/											\
			break;																							\
		}																									\
		if((itemptr)->nextfld) (itemptr)->nextfld->prevfld=(itemptr)->prevfld;								\
		if(!(uintptr_t((itemptr)->prevfld) & 1)) (itemptr)->prevfld->nextfld=(itemptr)->nextfld;			\
		else { /*this is first item, so it contains address of head pointer to be updated*/					\
			assert(*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) == itemptr); /*head must point to itemptr*/	\
			*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) = (itemptr)->nextfld;							\
		}																									\
	} while(0)

//clears next and prev pointers after removal from list, so entry looks disconnected
#define BILINKLIST_REMOVE(itemptr, nextfld, prevfld) \
	do {																									\
		BILINKLIST_REMOVE_NOCL(itemptr, nextfld, prevfld);													\
		(itemptr)->prevfld = (itemptr)->nextfld = NULL;														\
	} while(0)

//adds item to the head of list
#define BILINKLIST_INSERTHEAD(itemptr, headvar, nextfld, prevfld) \
	do {																									\
		/*itemptr and address of headvar must be aligned to some non-zero power of 2 bytes*/				\
		assert((uintptr_t(itemptr) & 1)==0 && (uintptr_t(&(headvar)) & 1)==0);								\
		if(headvar) (headvar)->prevfld=itemptr;																\
		(itemptr)->nextfld=headvar;																			\
		*((void**)(&((itemptr)->prevfld)))=(void*)(uintptr_t(&(headvar)) | 1);								\
		headvar=itemptr;																					\
	} while(0)

//adds item after another specific non-NULL item
#define BILINKLIST_INSERTAFTER(itemptr, afteritemptr, nextfld, prevfld) \
	do {																									\
		if((afteritemptr)->nextfld) (afteritemptr)->nextfld->prevfld=itemptr;								\
		(itemptr)->nextfld=(afteritemptr)->nextfld;															\
		(itemptr)->prevfld=afteritemptr;																	\
		(afteritemptr)->nextfld=itemptr;																	\
	} while(0)

//checks if item is at head of list
#define BILINKLIST_ISHEAD(itemptr, prevfld) ((uintptr_t((itemptr)->prevfld) & 1)!=0)

//replaces item at itemptr with item at newitemptr. newitemptr must be detached. itemptr IS NOT CLEARED, so nextfld and prevfld must be nullified manually is necessary!
#define BILINKLIST_REPLACE(itemptr, newitemptr, nextfld, prevfld) \
	do {																										\
		assert((newitemptr)->prevfld==NULL);																	\
		if(!(itemptr)->prevfld) break;																			\
		(newitemptr)->nextfld=(itemptr)->nextfld;																\
		(newitemptr)->prevfld=(itemptr)->prevfld;																\
		if((itemptr)->nextfld) (itemptr)->nextfld->prevfld=newitemptr;											\
		if((uintptr_t((itemptr)->prevfld) & 1)!=0) { /*is at head, so head pointer must be updated*/			\
			assert(*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) == itemptr); /*head must point to itemptr*/	\
			*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) = newitemptr;										\
		} else { /*not at head, so just update prev item to point to new item */								\
			(itemptr)->prevfld->nextfld=newitemptr;																\
		}																										\
	} while(0)

//fixes back pointer to head for first item which must be already pointed to by headvar
#define BILINKLIST_FIXHEAD(headvar, prevfld) \
	do {																									\
		/*address of headvar must be aligned to some non-zero power of 2 bytes*/							\
		if(!(headvar)) break;																				\
		assert((uintptr_t(&(headvar)) & 1)==0);																\
		*((void**)(&((headvar)->prevfld)))=(void*)(uintptr_t(&(headvar)) | 1);								\
	} while(0)


///////////////////////////////////////////////////////////////////////////////////////////////////////////
//Bi-linked list WITH TAIL (prev value of first item contains special mark and address of head variable, next value of last item contains special mark and address of tail variable)
//Have important features:
// - head and tail pointers are NULL for empty list
// - prev or next field is NULL for disconnected items only;
// - can be quickly removed from parent list without necessity to explicitely keep any identification of parent list

//variant without cleaning next and prev pointers to be used just before inserting into another bi-linked list or freeing
#define BILINKLISTWT_REMOVE_NOCL(itemptr, nextfld, prevfld) \
	do {																									\
		if(!(itemptr)->nextfld || !(itemptr)->prevfld) { /*disconnected item*/								\
			assert((itemptr)->nextfld==(itemptr)->prevfld); /*check that both pointers are NULL*/			\
			break;																							\
		}																									\
		if(!(uintptr_t((itemptr)->nextfld) & 1)) (itemptr)->nextfld->prevfld=(itemptr)->prevfld;/*not last*/	\
		else { /*this is last item, so it contains address of tail pointer to be updated*/					\
			assert(*((void**)(uintptr_t((itemptr)->nextfld) ^ 1)) == itemptr);/*tail must point to itemptr*/	\
			if(uintptr_t((itemptr)->prevfld) & 1) { /*this is also first item, so list becomes empty*/		\
				assert(*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) == itemptr);/*head must point to itemptr*/\
				*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) = NULL;/*head becomes NULL*/					\
				*((void**)(uintptr_t((itemptr)->nextfld) ^ 1)) = NULL;/*tail becomes NULL*/					\
				break;																						\
			}																								\
			*((void**)(uintptr_t((itemptr)->nextfld) ^ 1)) = (itemptr)->prevfld;							\
		}																									\
		if(!(uintptr_t((itemptr)->prevfld) & 1)) (itemptr)->prevfld->nextfld=(itemptr)->nextfld;/*not first*/	\
		else { /*this is first item, so it contains address of head pointer to be updated*/					\
			assert(*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) == itemptr); /*head must point to itemptr*/	\
			*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) = (itemptr)->nextfld;							\
		}																									\
	} while(0)

//clears next and prev pointers after removal from list, so entry looks disconnected
#define BILINKLISTWT_REMOVE(itemptr, nextfld, prevfld) \
	do {																									\
		BILINKLISTWT_REMOVE_NOCL(itemptr, nextfld, prevfld);												\
		(itemptr)->prevfld = (itemptr)->nextfld = NULL;														\
	} while(0)

//adds item to the head of list
#define BILINKLISTWT_INSERTHEAD(itemptr, headvar, tailvar, nextfld, prevfld) \
	do {																									\
		/*itemptr and address of headvar and tailvar must be aligned to some non-zero power of 2 bytes*/	\
		assert((uintptr_t(itemptr) & 1)==0 && (uintptr_t(&(headvar)) & 1)==0 && (uintptr_t(&(tailvar)) & 1)==0);\
		if(headvar) { /*list is not empty*/																	\
			(headvar)->prevfld=itemptr;																		\
			(itemptr)->nextfld=headvar;																		\
		} else { /*list is empty, so tail must be assigned*/												\
			tailvar=itemptr;																					\
			*((void**)(&((itemptr)->nextfld)))=(void*)(uintptr_t(&(tailvar)) | 1);							\
		}																									\
		*((void**)(&((itemptr)->prevfld)))=(void*)(uintptr_t(&(headvar)) | 1);								\
		headvar=itemptr;																					\
	} while(0)

//adds item to the tail of list
#define BILINKLISTWT_INSERTTAIL(itemptr, headvar, tailvar, nextfld, prevfld) \
	do {																									\
		/*itemptr and address of headvar and tailvar must be aligned to some non-zero power of 2 bytes*/	\
		assert((uintptr_t(itemptr) & 1)==0 && (uintptr_t(&(headvar)) & 1)==0 && (uintptr_t(&(tailvar)) & 1)==0);\
		if(tailvar) { /*list is not empty*/																	\
			(tailvar)->nextfld=itemptr;																		\
			(itemptr)->prevfld=tailvar;																		\
		} else { /*list is empty, so head must be assigned*/												\
			headvar=itemptr;																					\
			*((void**)(&((itemptr)->prevfld)))=(void*)(uintptr_t(&(headvar)) | 1);							\
		}																									\
		*((void**)(&((itemptr)->nextfld)))=(void*)(uintptr_t(&(tailvar)) | 1);								\
		tailvar=itemptr;																					\
	} while(0)




//Bitmaps built of arrays of uint32_t
static inline uint32_t bitmap32_test_bit(const uint32_t* _map, unsigned _bit) {
	return _map[_bit>>5] & (1U << (_bit & (32-1)));
}
static inline void bitmap32_set_bit(uint32_t* _map, unsigned _bit) {
	_map[_bit>>5] |= (1U << (_bit & (32-1)));
}
static inline void bitmap32_clear_bit(uint32_t* _map, unsigned _bit) {
	_map[_bit>>5] &= ~(1U << (_bit & (32-1)));
}

//representation of bitmap with continuously MANUALLY allocated storage using array of uint32 
class bitmap32 final {
	uint32_t size; //number of following uint32 numbers
	uint32_t bitmap[]; //pointer to array of size items
public:
	static size_t calc_size(uint32_t num_bits) { //caclculates necessary memory to allocate for this object and bitmap for num_bits
		if(!num_bits) {
			assert(false);
			return 0;
		}
		return sizeof(bitmap32) + sizeof(bitmap[0])*(((num_bits-1)>>5)+1);
	}
	bitmap32(uint32_t num_bits, bitmap32* src=NULL) : size(((num_bits-1)>>5)+1) { //num_bits must match same argument to calc_size() when caclculating memory for allocation
		assert(num_bits>0);
		if(src) {
			*this=*src;
		} else {
			clear();
		}
	}
	bool has_space(uint32_t num_bits) const { //checks if current object has space to kepp num_bits bits
		return size>=((num_bits-1)>>5)+1;
	}
	void set_bit(unsigned bit) {
		assert((bit>>5)<size);
		bitmap32_set_bit(bitmap, bit);
	}
	void clear_bit(unsigned bit) {
		assert((bit>>5)<size);
		bitmap32_clear_bit(bitmap, bit);
	}
	uint32_t test_bit(unsigned bit) const {
		assert((bit>>5)<size);
		return bitmap32_test_bit(bitmap, bit);
	}
	void clear(void) {
		memset(bitmap, 0, size*sizeof(bitmap[0]));
	}
	uint32_t count_bits(void) const {
		uint32_t i=size;
		uint32_t n=0;
		do {
			i--;
			n+=ecb_popcount32(bitmap[i]);
		} while(i>0);
		return n;
	}

	bitmap32& operator|=(const bitmap32& op) {
		uint32_t i = size<=op.size ? size : op.size;
		do {
			i--;
			bitmap[i]|=op.bitmap[i];
		} while(i>0);
		return *this;
	}
	bitmap32& operator^=(const bitmap32& op) {
		uint32_t i = size<=op.size ? size : op.size;
		do {
			i--;
			bitmap[i]^=op.bitmap[i];
		} while(i>0);
		return *this;
	}
	bitmap32& operator&=(const bitmap32& op) {
		uint32_t i;
		if(size>op.size) {
			memset(bitmap + size, 0, sizeof(bitmap[0])*(size-op.size));
			i = op.size;
		} else {
			i=size;
		}
		do {
			i--;
			bitmap[i]&=op.bitmap[i];
		} while(i>0);
		return *this;
	}
	bitmap32& operator=(const bitmap32& op) {
		uint32_t i;
		if(size>op.size) {
			memset(bitmap + size, 0, sizeof(bitmap[0])*(size-op.size));
			i = op.size;
		} else {
			i=size;
		}
		memcpy(bitmap, op.bitmap, sizeof(bitmap[0])*i);
		return *this;
	}

	bitmap32& operator-=(const bitmap32& op) {
		uint32_t i = size<=op.size ? size : op.size;
		do {
			i--;
			bitmap[i]&=~op.bitmap[i];
		} while(i>0);
		return *this;
	}
	bitmap32& operator~(void) {
		uint32_t i = size;
		do {
			i--;
			bitmap[i]=~bitmap[i];
		} while(i>0);
		return *this;
	}

	bool operator!(void) const {
		uint32_t i = size;
		do {
			i--;
			if(bitmap[i]) return false;
		} while(i>0);
		return true;
	}

	explicit operator bool(void) const {
		uint32_t i = size;
		do {
			i--;
			if(bitmap[i]) return true;
		} while(i>0);
		return false;
	}
};


/*
//Bitmaps built of arrays of uint16_t
static inline uint16_t bitmap16_test_bit(uint16_t* _map, unsigned _bit) {
	return _map[_bit>>4] & (1 << (_bit & (16-1)));
}
static inline void bitmap16_set_bit(uint16_t* _map, unsigned _bit) {
	_map[_bit>>4] |= (1 << (_bit & (16-1)));
}
static inline void bitmap16_clear_bit(uint16_t* _map, unsigned _bit) {
	_map[_bit>>4] &= ~(1 << (_bit & (16-1)));
}

//Bitmaps built of arrays of uint8_t
static inline uint8_t bitmap8_test_bit(uint8_t* _map, unsigned _bit) {
	return _map[_bit>>3] & (1U << (_bit & (8-1)));
}
static inline void bitmap8_set_bit(uint8_t* _map, unsigned _bit) {
	_map[_bit>>3] |= (1 << (_bit & (8-1)));
}
static inline void bitmap8_clear_bit(uint8_t* _map, unsigned _bit) {
	_map[_bit>>3] &= ~(1 << (_bit & (8-1)));
}
*/


//Class to manage chain of memory buffers and treat it like simple buffer
struct iot_membuf_chain;
struct iot_membuf_chain {
	iot_membuf_chain *next;
	uint32_t len, total_len; //len - size of useful space in this struct, total_len - sum of all 'len's in this and all following structs (connected with 'next')
	char buf[2]; //determines minimal allowed size of added buffer

	uint32_t init(uint32_t sz) { //sz - size of buffer on which this struct is put over
		if(sz<sizeof(iot_membuf_chain)) return 0;
		next=NULL;
		len=total_len=get_increment(sz);
		return len;
	}
	static unsigned int get_increment(uint32_t sz) { //for provided size of buffer returns increment of useful space
		if(sz<sizeof(iot_membuf_chain)) return 0;
		return sz-offsetof(iot_membuf_chain, buf);
	}
	uint32_t add_buf(char *newbuf, uint32_t sz) { //adds one more buffer to the end of chain. Operation is non-destructive for data in buf
		//returns 0 if provided buffer is too small or new total len otherwise
		iot_membuf_chain* n=(iot_membuf_chain *)newbuf;
		if(!n->init(sz)) return 0;
		uint32_t d=n->len;
		iot_membuf_chain* t=this;
		total_len+=d;
		while(t->next) {
			t=t->next;
			t->total_len+=d;
		}
		t->next=n;
		return total_len;
	}
	char *drop_nextbuf(void) {//removes next buf reference (thus decreasing chain length by 1), connecting its child to current struct as next. 
							//Destroys!!! data in buf if data length is larger than 'len'
		//returns removed buffer address or NULL if no next buffer and nothing was done
		if(!next) return NULL;
		iot_membuf_chain* t=next;
		next=t->next;
		total_len-=t->len;
		return (char*)t;
	}
	bool has_children(void) { //checks if there are any children
		return next!=NULL;
	}
	unsigned count_children(void) { //counts number of connected bufs
		unsigned c=0;
		iot_membuf_chain *p=next;
		while(p) {
			c++;
			p=p->next;
		}
		return c;
	}
};

//abstraction class over uv_timer_t
class iot_timer {
	uv_timer_t timer;
	uv_thread_t thread;
	void (*notify)(iot_timer*, void *);
	static void ontimer(uv_timer_t *w) {
		iot_timer* t=(iot_timer*)w->data;
		t->notify(t, t->param);
	}
public:
	void *param;
	iot_timer(void) {
		timer.loop=NULL;
		param=NULL;
		notify=NULL;
	}
	~iot_timer(void) {
		if(timer.loop && uv_is_active((uv_handle_t*)&timer)) { //inited and started
			stop();
		}
	}
	void init(uv_thread_t th=0) { // only in provided thread can start() and stop() be called. can be called several times if operating thread must be changed, but will assert if timer is still active in another thread
		if(!th) th=uv_thread_self();
		uv_loop_t *loop=kapi_get_event_loop(th);
		assert(loop!=NULL);

		if(timer.loop && uv_is_active((uv_handle_t*)&timer)) { //inited and started. hope thread is the same as was
			stop();
		}
		thread=th;
		uv_timer_init(loop, &timer);
		timer.data=this;
	}
	bool start(void (*notify_)(iot_timer*, void *), uint64_t timeout, uint64_t period=0) {
		if(!timer.loop || !notify_) return false;
		assert(thread==uv_thread_self());
		if(thread!=uv_thread_self()) return false;

		notify=notify_;
		uv_timer_start(&timer, iot_timer::ontimer, timeout, period);
		return true;
	}
	bool stop(void) {
		if(!timer.loop) return false;
		assert(thread==uv_thread_self());
		if(thread!=uv_thread_self()) return false;

		uv_timer_stop(&timer);
		return true;
	}
	bool is_active(void) {
		if(!timer.loop) return false;
		assert(thread==uv_thread_self());
		return uv_is_active((uv_handle_t*)&timer);
	}
};

class iot_atimer_item {
	friend class iot_atimer;
	iot_atimer_item *next, *prev;
	uint64_t timesout; //corelates with result of uv_now, so is not unix time
public:
	void (*notify)(void *);
	void* param;

	iot_atimer_item(void* param_=NULL, void (*notify_)(void *)=NULL) : notify(notify_), param(param_) {
		next=prev=NULL;
	}
	~iot_atimer_item(void) {
		unschedule();
	}
	void init(void* param_, void (*notify_)(void *)) {
		notify=notify_;
		param=param_;
		next=prev=NULL;
	}
	void unschedule(void) { //should not be called in thread other than of corresponding iot_atimer object OR under same lock when nothreadcheck is true
		BILINKLISTWT_REMOVE(this, next, prev);
	}
	bool is_on(void) { //checks if timer is scheduled
		return next!=NULL;
	}
	uint64_t get_timeout(void) const {
		return timesout;
	}
};

//Action timer which works with uv library. Manages queue of EQUAL relative timer events (period is set during init) using minimal resources. Size of queue is unlimited
class iot_atimer {
	iot_atimer_item *head, *tail;
	uv_timer_t timer;
	uv_thread_t thread;
	uint64_t period; //zero if no timer inited
	void (*on_deinit)(void *deinitarg);
	void* deinitarg;
	bool nothreadcheck;
public:
	iot_atimer(void) {
		period=0;
	}
	~iot_atimer(void) {
		deinit();
	}
	void init(uint64_t period_, uv_loop_t *loop, bool nothreadcheck_=false) { //when nothreadcheck true this means that caller code must ensure correct thread or use mutexes before all atimer methods
		//must be called in correct thread of provided loop!
		assert(period==0); //forbid double init
		assert(loop!=NULL);
		if(nothreadcheck_) {
			nothreadcheck=true;
			thread=0;
		} else {
			nothreadcheck=false;
			thread=uv_thread_self();
		}

		if(period_==0) period_=1;
		head=tail=NULL;
		uv_timer_init(loop, &timer);
		timer.data=this;

		period=period_;
		uv_timer_start(&timer, iot_atimer::ontimer, period_, period_);
	}
	void set_thread(uv_thread_t thread_) {
		if(!nothreadcheck) {
			thread=thread_;
		} else {
			assert(false);
		}
	}
	bool deinit(void (*on_deinit_)(void *arg)=NULL, void* arg=NULL) { //returns true if deinit finished, false if memory cannot be released until on_deinit is called (it can be NULL if no such notification is necessary)
		if(period) {
			while(head)
				head->unschedule(); //here head is moved to next item!!!

			on_deinit=on_deinit_;
			deinitarg=arg;
			uv_close((uv_handle_t*)&timer, [](uv_handle_t *handle)->void {
				iot_atimer* th=(iot_atimer*)handle->data;
				if(th->on_deinit) th->on_deinit(th->deinitarg);
			});
			period=0;
			return false;
		}
		return true;
	}
	static void ontimer(uv_timer_t *w) {
		iot_atimer* th=(iot_atimer*)w->data;

		uint64_t now=uv_now(w->loop);
		//loop through all items in queue while they seems to be timed out
		iot_atimer_item * cur;
		iot_atimer_item * &head=th->head;
		while(head && head->timesout <= now) {
			cur=head;
			cur->unschedule(); //here head is moved to next item!!!
			//signal about timeout
			cur->notify(cur->param);
		}
		if(head) { //restart timer
			uv_timer_set_repeat(w, head->timesout - now);
		} else {
			if(uv_timer_get_repeat(w)==th->period) return; //optimization actual for libuv to avoid unnecessary calls which already done before this callback
			uv_timer_set_repeat(w, th->period);
		}
		uv_timer_again(w);
	}
	void schedule(iot_atimer_item &it) { //must be mutex protected when nothreadcheck is true!
		assert(nothreadcheck || thread==uv_thread_self());
		assert(period!=0);
		assert(it.notify!=NULL);
		if(expect_false(!it.notify)) return;

		it.unschedule();
		it.timesout=uv_now(timer.loop)+period;
		//add to the tail
		BILINKLISTWT_INSERTTAIL(&it, head, tail, next, prev);
	}
};

//atomic-only non-recursive spinning lock implementation. Should be used for short-time locking only!!! (i.e. to read/write small regions of memory)
//WILL DEADLOCK IF USED RECURSIVELY!!!
class iot_spinlock {
	mutable volatile std::atomic_flag _lock=ATOMIC_FLAG_INIT; //lock to protect critical sections

public:
	void lock(void) const { //wait for lock mutex
		uint16_t c=1;
		while(_lock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
		}
	}
	void unlock(void) const { //free lock mutex
		_lock.clear(std::memory_order_release);
	}
	void assert_is_locked(void) { //ensures lock is locked
		assert(_lock.test_and_set(std::memory_order_acquire));
	}
};

//atomic-only recursive (up to 5 levels) spinning lock implementation. Should be used for short-time locking only!!! (i.e. to read/write small regions of memory)
class iot_spinrlock {
	mutable volatile std::atomic_flag _lock=ATOMIC_FLAG_INIT; //lock to protect critical sections
	mutable volatile uint8_t lock_recurs=0;
	mutable volatile uv_thread_t locked_by={};

public:
	void lock(void) const { //wait for lock mutex
		if(locked_by==uv_thread_self()) { //this thread already owns lock, just increase recursion level
			lock_recurs++;
			assert(lock_recurs<5); //limit recursion to protect against endless loops
			return;
		}
		uint16_t c=1;
		while(_lock.test_and_set(std::memory_order_acquire)) {
			//busy wait
			if(!(c++ & 1023)) sched_yield();
		}
		locked_by=uv_thread_self();
		assert(lock_recurs==0);
	}
	void unlock(void) const { //free lock mutex
		if(locked_by!=uv_thread_self()) {
			assert(false);
			return;
		}
		if(lock_recurs>0) { //in recursion
			lock_recurs--;
			return;
		}
		locked_by={};
		_lock.clear(std::memory_order_release);
	}
	void assert_is_locked(void) { //ensures lock is locked
		assert(_lock.test_and_set(std::memory_order_acquire));
	}
};


class iot_objectrefable;
typedef void (*object_destroysub_t)(iot_objectrefable*);

void object_destroysub_memblock(iot_objectrefable*); //calls destructor and then iot_release_memblock
void object_destroysub_delete(iot_objectrefable*); //calls delete
void object_destroysub_staticmem(iot_objectrefable*); //calls just destructor


//object which can be referenced by iot_objid_t - derived struct, has reference count and delayed destruction
class iot_objectrefable {
protected:
	iot_spinrlock reflock;
private:
//reflock protected:
	mutable volatile int32_t refcount; //how many times address of this object is referenced. value -1 together with true pend_destroy means that destroy_sub was already called (or is right to be called)
	mutable volatile uint8_t pend_destroy; //flag that this struct in waiting for zero in refcount to be freed. can be accessed under acclock only
#ifndef NDEBUG
public:
	mutable uint8_t debug=0; //can be manually set to true after object creation to get debug output about every ref() and unref() calls
#endif
///////////////////

	object_destroysub_t destroy_sub; //can be NULL if no specific destruction required (i.e. object is statically allocated). this function MUST call
		//iot_objectrefable destructor (explicitely or doing delete)

protected:
	iot_objectrefable(void) = delete;
	iot_objectrefable(const iot_objectrefable&) = delete;
	iot_objectrefable(object_destroysub_t destroy_sub, bool is_dynamic) : destroy_sub(destroy_sub) {
		if(is_dynamic) { //creates object with 1 reference count and pending release, so first unref() without prior ref() will destroy the object
			refcount=1;
			pend_destroy=1;
		} else { //such mode can be used for statically allocated objects (in such case destroy_sub must be NULL)
			refcount=0;
			pend_destroy=0;
		}
	}
public:
	virtual ~iot_objectrefable(void) {
		if(!destroy_sub) { //statically allocated global object, so no explicit try_destroy call required to be done and refcount must be 0 on destruction in such case
			if(refcount==0) return;
			//else try_destroy call could have been done and refcount must be -1
		}
		assert(refcount==-1 && pend_destroy); //ensure destroy was called (and thus refcount checked)
	}
	int32_t get_refcount(void) const {
		return refcount;
	}
	void ref(void) const { //increment refcount if destruction is not pending. returns false in last case
//outlog_error("%x ref increased", unsigned(uintptr_t(this)));
		reflock.lock();
		assert(refcount>=0);
//		if(refcount<0) {
//			assert(false);
//			pend_destroy=true; //set to make guaranted exit by next condition
//		}
//		if(pend_destroy) { //cannot be referenced ones more
//			reflock.unlock();
//			return false;
//		}
		refcount++;

#ifndef NDEBUG
		if(debug) {
			void* tmp[4]; //we need to abandon first value. it is always inside this func
			int nback;
			nback=backtrace(tmp, 4);
			if(nback>1) {
				char **symb=backtrace_symbols(tmp+1,3);
#ifndef DAEMON_CORE
				kapi_outlog_notice("Object 0x%lx refered (now %d times) from %s <- %s <- %s", long(uintptr_t(this)), refcount, symb[0] ? symb[0] : "NULL", symb[1] ? symb[1] : "NULL", symb[2] ? symb[2] : "NULL");
#else
				outlog_notice("Object 0x%lx refered (now %d times) from %s <- %s <- %s", long(uintptr_t(this)), refcount, symb[0] ? symb[0] : "NULL", symb[1] ? symb[1] : "NULL", symb[2] ? symb[2] : "NULL");
#endif
				free(symb);
			}
		}
#endif
		reflock.unlock();
		return;// true;
	}
	void unref(void) {
//outlog_error("%x unref", unsigned(uintptr_t(this)));
		reflock.lock();
		if(refcount<=0) {
			assert(false);
			reflock.unlock();
			return;
		}
		refcount--;
#ifndef NDEBUG
		if(debug) {
			void* tmp[4]; //we need to abandon first value. it is always inside this func
			int nback;
			nback=backtrace(tmp, 4);
			if(nback>1) {
				char **symb=backtrace_symbols(tmp+1,3);
#ifndef DAEMON_CORE
				kapi_outlog_notice("Object 0x%lx UNrefered (now %d times) from %s <- %s <- %s", long(uintptr_t(this)), refcount, symb[0] ? symb[0] : "NULL", symb[1] ? symb[1] : "NULL", symb[2] ? symb[2] : "NULL");
#else
				outlog_notice("Object 0x%lx UNrefered (now %d times) from %s <- %s <- %s", long(uintptr_t(this)), refcount, symb[0] ? symb[0] : "NULL", symb[1] ? symb[1] : "NULL", symb[2] ? symb[2] : "NULL");
#endif
				free(symb);
			}
		}
#endif
		if(refcount>0 || !pend_destroy) {
			reflock.unlock();
			return;
		}
		//here refcount==0 and pend_destroy==true
		//mark as destroyed by setting refcount to -1
		refcount=-1;

		reflock.unlock();
		if(destroy_sub) destroy_sub(this);
	}
	bool try_destroy(void) { //destroys object by calling destroy_sub and returns true if refcount is zero. otherwise marks object to be destroyed on last reference free and returns false
		reflock.lock();
		pend_destroy=1;
		if(refcount==0) {
			//mark as destroyed by setting refcount to -1
			refcount=-1;
			reflock.unlock();
			if(destroy_sub) destroy_sub(this);
			return true;
		}
		assert(refcount>0);
		reflock.unlock();
		return false;
	}
};



//smart pointer which holds additional reference to iot_objectrefable-derived objects and frees it on destruction
template<typename T> class iot_objref_ptr {
	T *ptr;
public:
	iot_objref_ptr(void) : ptr(NULL) {} //default constructor for arrays
	iot_objref_ptr(const iot_objref_ptr& src) { //copy constructor
		ptr=src.ptr;
		if(ptr) ptr->ref(); //increase refcount
	}
	iot_objref_ptr(iot_objref_ptr&& src) noexcept { //move constructor
		ptr=src.ptr;
		src.ptr=NULL;
	}
	iot_objref_ptr(T *obj) {
		if(!obj) {
			ptr=NULL;
		} else {
			ptr=obj;
			ptr->ref(); //increase refcount
		}
	}
	iot_objref_ptr(bool noref, T *obj) { //true 'noref' tells to not increment refcount
		if(!obj) {
			ptr=NULL;
		} else {
			ptr=obj;
			if(!noref) ptr->ref(); //increase refcount
		}
	}
	iot_objref_ptr(T && srcobj) = delete; //move constructor for temporary object. NO TEMPORARY iot_objectrefable-derived OBJECTS SHOULD BE CREATED

	~iot_objref_ptr() {
		clear();
	}
	void clear(bool nounref=false) { //free reference
		if(!ptr) return;
		if(!nounref) ptr->unref();
		ptr=NULL;
	}
	iot_objref_ptr& operator=(T *obj) {
		if(ptr==obj) return *this; //no action
		if(ptr) ptr->unref();
		if(!obj) {
			ptr=NULL;
		} else {
			ptr=obj;
			ptr->ref(); //increase refcount
		}
		return *this;
	}
	iot_objref_ptr& operator=(const iot_objref_ptr& src) { //copy assignment
		if(this==&src || ptr==src.ptr) return *this; //no action
		if(ptr) ptr->unref();
		ptr=src.ptr;
		if(ptr) ptr->ref(); //increase refcount
		return *this;
	}
	iot_objref_ptr& operator=(iot_objref_ptr&& src) {  //move assignment
		if(this==&src || ptr==src.ptr) return *this; //no action
		if(ptr) ptr->unref();
		ptr=src.ptr;
		src.ptr=NULL;
		return *this;
	}
	bool operator==(const iot_objref_ptr& rhs) const{
		return ptr==rhs.ptr;
	}
	bool operator!=(const iot_objref_ptr& rhs) const{
		return ptr!=rhs.ptr;
	}
	bool operator==(const T *rhs) const{
		return ptr==rhs;
	}
	bool operator!=(const T *rhs) const{
		return ptr!=rhs;
	}
	explicit operator bool() const {
		return ptr!=NULL;
	}
	bool operator !(void) const {
		return ptr==NULL;
	}
	explicit operator T*() const {
		return ptr;
	}
	T* operator->() const {
		assert(ptr!=NULL);
		return ptr;
	}
};













template <bool shared> class iot_fixprec_timer;

template <bool shared> class iot_fixprec_timer_item {
	friend class iot_fixprec_timer<shared>;
	iot_fixprec_timer_item *next=NULL, *prev=NULL;
	volatile std::atomic<iot_fixprec_timer<shared> *>timer={NULL};
public:
	void (*notify)(void *param, uint32_t period_id, uint64_t now_ms);
	void* param;

	iot_fixprec_timer_item(void* param_=NULL, void (*notify_)(void *param, uint32_t period_id, uint64_t now_ms)=NULL) : notify(notify_), param(param_) {
	}
	~iot_fixprec_timer_item(void) {
		unschedule();
	}
	void init(void* param_, void (*notify_)(void *param, uint32_t period_id, uint64_t now_ms)) {
		notify=notify_;
		param=param_;
	}
	void unschedule(void) {
		if(shared) {
			auto t=timer.exchange((iot_fixprec_timer<shared> *)1, std::memory_order_acq_rel); //value 1 is used to lock item structure against unschedule invocations in other threads
			if(t==(iot_fixprec_timer<shared> *)1) return; //item is locked in unschedule

			if(t) {//is on
				t->unschedule(this);
			} //else is off
			//unlock
			timer.store(NULL, std::memory_order_release);
		} else {
			auto t=timer.load(std::memory_order_relaxed);
			if(t) {
				t->unschedule(this);
				timer.store(NULL, std::memory_order_relaxed);
			}
		}
//		BILINKLISTWT_REMOVE(this, next, prev);
	}
	bool is_on(void) { //checks if timer is scheduled
		return timer.load(shared ? std::memory_order_acquire : std::memory_order_relaxed)!=NULL;
	}
};

class iot_thread_item_t;

//Fixed precision action timer which works with uv library.
//Manages queue of arbitrary relative timer events with fixed precision P(set during init) and maximum delay MD.
//So a timer with precision 100ms will round 49ms to 0ms and 50ms to 100ms.
//Relation MD/P+1=N determines size of allocated memory. Now N is limited to 2048
template <bool shared> class iot_fixprec_timer {
	friend class iot_fixprec_timer_item<shared>;

	iot_fixprec_timer_item<shared> **wheelbuf=NULL;
	uint32_t prec; //precision in ms
	uint32_t maxerror; //maximum timer error in ms
	uint64_t maxdelay; //maximum delay in ms
	uint32_t numitems=0; //number of items in wheelbuf
	uint32_t curitem;
	uint32_t period_id;
	uint64_t starttime;
	uv_timer_t timer;
	iot_thread_item_t *thread=NULL;
	uv_loop_t *loop=NULL;
	iot_spinlock lock;
	void (*on_deinit)(void *deinitarg)=NULL;
	void* deinitarg=NULL;
public:
	iot_fixprec_timer(void) {
	}
	~iot_fixprec_timer(void) {
		deinit();
	}
	bool thread_valid(void);// {
//		return uv_thread_self()==thread->thread;
//	}
	int init(uint32_t prec_, uint64_t maxdelay_);
	bool is_init(void) const {
		return wheelbuf!=NULL;
	}
	bool deinit(void (*on_deinit_)(void *arg)=NULL, void* arg=NULL) { //returns true if deinit finished, false if memory cannot be released until on_deinit is called (it can be NULL if no such notification is necessary)
		if(!wheelbuf) return true;
		for(uint32_t i=0;i<numitems;i++) {
			iot_fixprec_timer_item<shared> * &head=wheelbuf[i];
			while(head) head->unschedule(); //here head is moved to next item!!!
		}

		on_deinit=on_deinit_;
		deinitarg=arg;
		uv_close((uv_handle_t*)&timer, [](uv_handle_t *handle)->void {
			iot_fixprec_timer* th=(iot_fixprec_timer*)handle->data;
			if(th->on_deinit) th->on_deinit(th->deinitarg);
		});
		iot_release_memblock(wheelbuf);
		wheelbuf=NULL;
		return false;
	}
	void ontimer(void) {
		uint64_t now=uv_now(loop);
		//check if timer became too inaccurate
		uint64_t needtime=starttime + uint64_t(curitem)*prec; //now must be about such time
		uint32_t steps=1;
		if(needtime<now) {
			if(now-needtime>maxerror) {
				steps+=uint32_t((now-needtime+prec/2)/prec); //timer is lagging behind. process several items
				if(steps>numitems || now-needtime>0xffffffff) steps=numitems;
			}
		} else if(needtime>now && needtime-now>maxerror) return; //timer went ahead, skip one iteration


		iot_fixprec_timer_item<shared> * head;

		if(shared) {
			lock.lock();
			uint32_t tmpcuritem=curitem;
			uint32_t tmpperiod_id=period_id;
			//set correct curitem and period_id before calling any notify() because it can reschedule timer and must do this with correct delay relative to time of moment AFTER all steps are processed
			curitem+=steps; //increment curitem BEFORE running notify() because otherwise rescheduling timer with zero delay will readd item to the same linked list and cause enless loop
			period_id+=steps;
			if(curitem>=numitems) {
				curitem-=numitems;
				starttime=now+prec-uint64_t(curitem)*prec; //realign starttime when curitem passes 0
			}

			while(steps>0) {
again:
				head=wheelbuf[tmpcuritem];
				if(head) {
					BILINKLIST_REMOVE(head, next, prev);
					lock.unlock();
					head->timer.store(NULL, std::memory_order_release);
					head->notify(head->param, tmpperiod_id, now); //reschedule() called from notify() will see updated curitem and period_id
					lock.lock();
					goto again;
				}
				tmpcuritem++;
				tmpperiod_id++;
				if(tmpcuritem>=numitems) tmpcuritem=0;
				steps--;
			}
			lock.unlock();
		} else {
			assert(thread_valid());
			uint32_t tmpcuritem=curitem;
			uint32_t tmpperiod_id=period_id;
			//set correct curitem and period_id before calling any notify() because it can reschedule timer and must do this with correct delay relative to time of moment AFTER all steps are processed
			curitem+=steps; //increment curitem BEFORE running notify() because otherwise rescheduling timer with zero delay will readd item to the same linked list and cause enless loop
			period_id+=steps;
			if(curitem>=numitems) {
				curitem-=numitems;
				starttime=now+prec-uint64_t(curitem)*prec; //realign starttime when curitem passes 0
			}

			while(steps>0) {
				while((head=wheelbuf[tmpcuritem])) {
					BILINKLIST_REMOVE(head, next, prev);
					head->timer.store(NULL, std::memory_order_relaxed);
					head->notify(head->param, tmpperiod_id, now);
				}
				tmpcuritem++;
				tmpperiod_id++;
				if(tmpcuritem>=numitems) tmpcuritem=0;
				steps--;
			}
		}
	}
	//errors:
	//IOT_ERROR_INVALID_ARGS  - passed timer item was not inited
	//IOT_ERROR_ACTION_CANCELLED - for shared timer concurrent schedule() was running in another thread
	int schedule(iot_fixprec_timer_item<shared> &it, uint64_t delay, uint32_t *pper_id=NULL) { //delay in ms
		//when no errors and per_id is not NULL, it is filled with period ID at which item will be notified if won't be updated/cancelled
		if(!wheelbuf) return IOT_ERROR_NOT_INITED;
		assert(numitems>0);
		if(expect_false(!it.notify)){
			assert(false);
			return IOT_ERROR_INVALID_ARGS;
		}
		if(expect_false(delay>maxdelay)) delay=maxdelay;
		uint32_t relidx=uint32_t((delay + prec/2) / prec);

		it.unschedule();

		if(shared) {
			lock.lock();
			uint16_t c=1;
			iot_fixprec_timer *t;
			do {
				t=NULL;
				if(it.timer.compare_exchange_strong(t, this, std::memory_order_acq_rel, std::memory_order_acquire)) break;
				if(t==(iot_fixprec_timer *)1) { //is locked in unschedule, just spin waiting for NULL or valid pointer
					//busy wait
					if(!(c++ & 1023)) sched_yield();
					continue;
				}
				//valid pointer was assigned by concurrent schedule() of another timer, exit
				lock.unlock();
				return IOT_ERROR_ACTION_CANCELLED;
			} while(1);
		} else {
			assert(thread_valid());
		}
		uint32_t per_id=relidx+period_id;
		auto &head=wheelbuf[(curitem+relidx) % numitems];

		BILINKLIST_INSERTHEAD(&it, head, next, prev);

		if(shared) lock.unlock();
		if(pper_id) *pper_id=per_id;
		return 0;
	}
private:
	void unschedule(iot_fixprec_timer_item<shared> *it) {
		if(shared) {
			lock.lock();
		} else {
			assert(thread_valid());
		}

		BILINKLIST_REMOVE(it, next, prev);

		if(shared) lock.unlock();
	}
};








//Keeps references to allocated (using malloc()) memory blocks.
class mempool {
	void **memchunks=NULL; //array of allocated memory chunks
	int nummemchunks=0, maxmemchunks=0; //current quantity of chunks pointers in memchunks, number of allocated items in memchunks array

public:
	~mempool(void) {
		deinit();
	}
	void *allocate(uint32_t size, bool zero=false) { //allocate next chunk of memory
	//returns NULL on allocation error
		if(nummemchunks+1>maxmemchunks) { //reallocate memchunks array to make it bigger
			int newmax=nummemchunks+1+50;
			void **t=(void**)malloc(sizeof(void*)*newmax);
			if(!t) return NULL;
			if(nummemchunks>0) memmove(t, memchunks, sizeof(void*)*nummemchunks);
			if(memchunks) free(memchunks);
			memchunks=t;
			maxmemchunks=newmax;
		}
		void *t=malloc(size);
		if(!t) return NULL;
		memchunks[nummemchunks]=t;
		nummemchunks++;
		if(zero) memset(t, 0, size);
		return t;
	}
	void deinit(void) { //free all allocated chunks
		if(!memchunks) return;
		for(int i=0;i<nummemchunks;i++) free(memchunks[i]);
		free(memchunks);
		memchunks=NULL;
		nummemchunks=0;
		maxmemchunks=0;
	}
};


//allocate specified minimal quantity of items and add them to uni- or bidirectional list whose head is in 'ret' (updating 'ret' as necessary)
template<class Item, bool UsePrev=false, uint32_t OPTIMAL_BLOCK=256*1024, uint32_t MAX_BLOCK=2*1024*1024> inline bool alloc_free_items(mempool* mpool, uint32_t &n, uint32_t sz, Item * &ret, uint32_t maxn=0xFFFFFFFFu) {
	//'n' - minimal amount to be allocated for success. on exit it is updated to show quantity of allocated items
	//'sz' - size of each item (if Item is base class for allocated items then this size can be larger than sizeof(Item))
	//'ret' - head of allocated unidirectional list of items will be put here. if 'ret' already has pointer to unidirectional list, this list will be prepended
	//'maxn' - optional maximum quantity of allocated items
	//returns false if 'n' was not satisfied (but less structs can be allocated and returned with 'n' updated to show quantity of allocated)
		uint32_t perchunk;
		uint32_t nchunks;
		uint32_t chunksize;
		
		if(n>maxn) n=maxn;

		perchunk=OPTIMAL_BLOCK/sz;
		if(perchunk < n) {
			//optimal block is not enough for n items, try to take bigger chunk up to MAX_BLOCK
			chunksize=sz*n;
			if(chunksize>MAX_BLOCK && sz<MAX_BLOCK) { //avoid allocation chunks larger than MAX_BLOCK
				perchunk=MAX_BLOCK/sz;
				chunksize=perchunk*sz; //will be >OPTIMAL_BLOCK and <=MAX_BLOCK
				nchunks=(n+perchunk-1)/perchunk; //emulate integer ceil()
				//chunks must be recalculated after first allocation
			} else {
				nchunks=1;
				perchunk=n;
			}
		} else { //optimal block is enough for n and more
			nchunks=1;
			if(perchunk>maxn) perchunk=maxn;
			chunksize=perchunk*sz;
		}

		uint32_t n_good=0;
		while(nchunks>0) {
			char *t=(char*)mpool->allocate(chunksize);
			if(!t) { //malloc failure
				if(n_good>=n || perchunk<=1) break; //stop if minimum quantity reached or chunksize cannot be decreased
				perchunk>>=1; //decrease chunksize to have the half of items
				chunksize=perchunk*sz;
				nchunks=((n-n_good)+perchunk-1)/perchunk;
				continue;
			}
			Item *first=(Item *)t;
			if(UsePrev) {
				Item* prevt=NULL;
				for(uint32_t i=0;i<perchunk-1;i++) {
					((Item *)t)->init_allocated_item((Item *)(t+sz), prevt);
					prevt=(Item *)t;
					t+=sz;
				}
				((Item *)t)->init_allocated_item(ret, prevt);
				if(ret) ret->set_allocated_item_prev((Item *)t);
			} else {
				for(uint32_t i=0;i<perchunk-1;i++) {
					((Item *)t)->init_allocated_item((Item *)(t+sz));
					t+=sz;
				}
				((Item *)t)->init_allocated_item(ret);
			}
			ret=first;
			nchunks--;
			n_good+=perchunk;
			if(nchunks>0 && n_good+perchunk>maxn) { //another samesized chunk will be allocated which overflows maxn
				assert(n_good<=maxn); //problem with some math above
				perchunk=maxn-n_good;
				if(!perchunk) break; //maxn reached
				nchunks=1;
				chunksize=perchunk*sz;
			}
		}
		if(n_good>=n) {
			n=n_good;
			return true;
		}
		n=n_good;
		return false;
	}


#endif //IOT_UTILS_H