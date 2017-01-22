#ifndef IOT_UTILS_H
#define IOT_UTILS_H

#include <assert.h>
#include <stddef.h>


#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))


//Uni-linked list without tail (NULL value of next field tells about EOL). headvar is of the same type as itemptr. Empty list has headvar==NULL
//Insert item with address itemptr at head of list. nextfld - name of field inside item struct for linking to next item
#define ULINKLIST_INSERTHEAD(itemptr, headvar, nextfld)	\
	do {												\
		(itemptr)->nextfld = headvar;					\
		headvar = itemptr;								\
	} while(0)

#define ULINKLIST_REMOVE(itemptr, headvar, nextfld)	\
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



//Bi-linked list without tail (NULL value of next field tells about EOL, prev value of first item contains special mark and address of head variable)
//Have important features:
// - prev field is NULL for disconnected items only;
// - can be quickly removed from parent list without necessity to explicitely keep any identification of parent list

//variant without cleaning next and prev pointers to be used just before inserting into another bi-linked list or freeing
#define BILINKLIST_REMOVE_NOCL(itemptr, nextfld, prevfld) \
	do {																									\
		if(!(itemptr)->prevfld) { /*disconnected item*/														\
			assert((itemptr)->nextfld==NULL); /*cannot have next*/											\
			return;																							\
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
		if(!(itemptr)->prevfld) { /*disconnected item*/														\
			assert((itemptr)->nextfld==NULL); /*cannot have next*/											\
			return;																							\
		}																									\
		if((itemptr)->nextfld) (itemptr)->nextfld->prevfld=(itemptr)->prevfld;								\
		if(!(uintptr_t((itemptr)->prevfld) & 1)) (itemptr)->prevfld->nextfld=(itemptr)->nextfld;			\
		else { /*this is first item, so it contains address of head pointer to be updated*/					\
			assert(*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) == itemptr); /*head must point to itemptr*/	\
			*((void**)(uintptr_t((itemptr)->prevfld) ^ 1)) = (itemptr)->nextfld;							\
		}																									\
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



//Bitmaps built of arrays of uint32_t
static inline uint32_t bitmap32_test_bit(uint32_t* _map, unsigned _bit) {
	return _map[_bit>>5] & (1U << (_bit & (32-1)));
}
static inline void bitmap32_set_bit(uint32_t* _map, unsigned _bit) {
	_map[_bit>>5] |= (1U << (_bit & (32-1)));
}
static inline void bitmap32_clear_bit(uint32_t* _map, unsigned _bit) {
	_map[_bit>>5] &= ~(1U << (_bit & (32-1)));
}

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




#endif //IOT_UTILS_H