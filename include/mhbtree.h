#ifndef MHBTREE_H
#define MHBTREE_H

/*  Required in code that uses this class
#ifndef _WIN32
    #undef _GNU_SOURCE
    #define _XOPEN_SOURCE 700
    #define _BSD_SOURCE
#	ifndef __APPLE__
	#include <features.h>
#	else
#	define __always_inline
#	endif
#else
	#define __always_inline
//	#include <malloc.h>
#endif

#include "commonbase.hpp"
*/

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/*
Template class which realizes in-Memory Hashed B+Tree storage.
Can be used to store just keys.
"Hashed" means that actually several independent trees will be created based on provided bit mask
Template args:
	Val_t - type name for Values. For complex values (objects) corresponding pointer type should be used to optimize memory used by tree.
				But this is not a requirement. If key-only storage is necessary, use predefined 'nullT' type name with zero size
	Key_t - type name for Keys. For complex values (objects or strings) use new type with embedded pointer to key object. This allows to redefine
				necessary operators:
		uint32_t operator& (uint32_t mask)  - must emulate applying of bitmask to a number and return uint32_t which after applying '>> bitsoffset' operation
											gives result with range [0 ; 1<<bits ) to give sub-tree index
		bool operator == (const Key_t& op)	- must check if two keys are equal
		bool operator < (const Key_T& op)	- must check if left key is less than right
		Key_t& operator = (int) 			- to initialize key with zero
	Bits - number of set contiguous bits in bitmask which selects sub-tree. Allowed range [0 ; 24]
	Bitsoffset - offset of contiguous bits in bitmask which selects sub-tree. Allowed range [0; 32-Bits]
	Highbits - should be true if order of keys (according to 'operator <') is the same as order of results of applying bitmask to keys. i.e. in case of integer keys
				the highest bits are used for sub-tree selection (e.g. for uint32 if 'Bits'==8 then 'Bitsoffset' must be 24 (32-8) for ability to use 'Highbits').
				Used just to optimize find_mult() call.
	MAX_TREE_DEPTH - maximum depth of each subtree. Together with constructor argument blocksize determines maximum quantity of elements in tree.
*/


//type with zero sizeof(). Can be used as Val_t class to get key-only storage
struct nullT{
	char dummy[0];
};


#define MHBTREE_NUM_PERBRANCH(bsize) ((((bsize)-offsetof(struct treeblock, branch.data)) / sizeof(branchitem)))
#define MHBTREE_NUM_PERLEAF(bsize) ((((bsize)-offsetof(struct treeblock, leaf.leaf)) / sizeof(leafitem)))

//T - type of stored data. use nullT type if no values must be stored
//K - type of key
// K must realize such operators:
template<class Val_t,class Key_t, uint32_t Bits, uint32_t Bitsoffset=0, bool Highbits=false, uint32_t MAX_TREE_DEPTH=10> class MemHBTree { //in-memory hbtree index
	const uint32_t bsize, numperbranch, numperleaf;
	uint32_t numfreeblocks;
	uint64_t amount;
	mempool mpool;
//each tree level occupies bsize bytes. first by has flag in bit 0, indicating if this block is a leaf. bytes 3 and 4 contain number of elements in current block
	struct treeblock;
public: //find_mult requires to export leafitem type
	struct pathitem {
		treeblock* offset;
		int idx;
	};
	struct treepath {
		pathitem pathway[MAX_TREE_DEPTH];
		int pathlen=-1;
		uint32_t root_idx; //index of root for pathway[0] block
	};
#pragma pack(4)
	struct leafitem {
		Key_t key;
		Val_t val;
	};
#pragma pack()
private:
#pragma pack(4)
	struct branchitem {
		treeblock* offset;
		Key_t key;
	};
	struct treeblock {
		char isleaf, _dummy; //0 for branch, 1 for leaf, -1 for free
		uint16_t num;
		union {
			struct {
				treeblock* left;
				branchitem data[];
			} branch; //branches
			struct {
				leafitem leaf[];
			} leaf;
			treeblock* next; //for free items
		};
		void init_allocated_item(treeblock* _next, treeblock* prev=NULL) {
			isleaf=-1;
			next=_next;
		}
		void set_allocated_item_prev(treeblock* prev=NULL) {
		}
	} **roots,*freeblockshead;
#pragma pack()
//if not found returns index of element, after which a new one must be inserted(-1 mean before all)
	bool find_key_inbranch(Key_t const &key,treeblock* bl,int &index,int start=0) {
		int lim = bl->num-start;
		if(lim<=0) {index=start-1;return false;}
		branchitem *base = bl->branch.data+start,* p;
		for (; lim != 0; lim >>= 1) {//size of current range, divided by 2 after each step
			p = base + (lim >> 1);//pointer to middle of current range
			if(p->key==key) {index=p-bl->branch.data;return true;}//check that we found our key
			if(p->key < key) {  /* key > p: move right */
				base = p + 1;
				lim--;
			}               /* else move left */
		}
		index=p-bl->branch.data;
		if(key < p->key) index--;
		return false;
	}
//if not found returns index of element, after which a new one must be inserted(-1 mean before all)
	bool find_key_inleaf(Key_t const &key,treeblock* bl,int &index, int start=0) {
		int lim = bl->num-start;
		if(lim<=0) {index=start-1;return false;}
		leafitem *base = bl->leaf.leaf+start,* p;
		for (; lim != 0; lim >>= 1) {//size of current range, divided by 2 after each step
			p = base + (lim >> 1);//pointer to middle of current range
			if(p->key==key) {index=p-bl->leaf.leaf;return true;}//check that we found our key
			if(p->key < key) {  /* key > p: move right */
				base = p + 1;
				lim--;
			}               /* else move left */
		}
		index=p-bl->leaf.leaf;
		if(key < p->key) index--;
		return false;
	}
	treeblock* get_freeblock(void) {
		if(!freeblockshead) return NULL; //blocks must be preallocated
		treeblock* res=freeblockshead;
		freeblockshead=freeblockshead->next;
		numfreeblocks--;
		return res;
	}
	void return_freeblock(treeblock* b) {
		b->isleaf=-1;
		b->next=freeblockshead;
		freeblockshead=b;
		numfreeblocks++;
	}
public:
	uint64_t getamount(void) const {
		return amount;
	}
	bool is_valid(void) const { //returns true if construction of tree was successful
		return roots!=NULL;
	}
	~MemHBTree(void) {
		if(!roots) return;
		removeall();
		roots=NULL;
		amount=0;
	}
	MemHBTree(uint32_t blocksize=512, uint32_t prealloc_q=0) : //size of each tree block, amount of items to preallocate tree blocks for
			bsize( MHBTREE_NUM_PERBRANCH(blocksize)>=4 && MHBTREE_NUM_PERLEAF(blocksize)>=4 ? blocksize :
				sizeof(branchitem)*4+offsetof(struct treeblock, branch.data) >= sizeof(leafitem)*4+offsetof(struct treeblock, leaf.leaf) ?
					sizeof(branchitem)*4+offsetof(struct treeblock, branch.data) : sizeof(leafitem)*4+offsetof(struct treeblock, leaf.leaf)),
			numperbranch(MHBTREE_NUM_PERBRANCH(bsize) > 65535 ? 65535 : MHBTREE_NUM_PERBRANCH(bsize)),
			numperleaf(MHBTREE_NUM_PERLEAF(bsize) > 65535 ? 65535 : MHBTREE_NUM_PERLEAF(bsize))
//			bsize(blocksize),
//			numperbranch((bsize-offsetof(struct treeblock, branch.data)) / sizeof(branchitem)),
//			numperleaf((bsize-offsetof(struct treeblock, leaf.leaf)) / sizeof(leafitem))
	{
		amount=0;
		freeblockshead=NULL;
		numfreeblocks=0;
		roots=(treeblock**)mpool.allocate((1u << Bits)*sizeof(treeblock*), true);
		if(!roots) return; //leave in invalid state
		if(prealloc_q>0) {
			uint32_t n=(prealloc_q * 2 / numperleaf) * (numperbranch+1) / numperbranch;
			alloc_free_items<treeblock>(&mpool, n, bsize, freeblockshead);
			numfreeblocks=n;
		}
	}
	int get_first(const Key_t **key,Val_t** old_data,treepath& path,uint32_t start_treeidx=0) { //finds first key (in order according to subtrees and 'operator <') in tree. if found and sizeof(Val_t)>0 then saves pointer to data to old_data
		//path object must be created and passed here
		//start_treeidx used internally by get_next()
		//returns 1-found,0-nothing found (no items),-2 - error to large depth;
		if(amount==0) {
			path.pathlen=-1;
			return 0;
		}
		treeblock* record_addr=NULL;
		for(uint32_t i=start_treeidx;i<(1u << Bits);i++) {
			if(!roots[i] || !roots[i]->num) continue;
			record_addr=roots[i];
			path.root_idx=i;
			break;
		}
		if(!record_addr) { //no items
			assert(start_treeidx>0); //amount should be 0 and thus condition impossible with zero start_treeidx
			path.pathlen=-1;
			return 0;
		}
		//begin search in the tree
		for(uint32_t path_len=0;path_len<MAX_TREE_DEPTH;path_len++) {
			path.pathway[path_len].offset=record_addr;
			if(record_addr->isleaf==1) { //leaf level, the deepest possible
				path.pathlen=(int)path_len;
				if(sizeof(Val_t)>0 && old_data) *old_data=&record_addr->leaf.leaf[0].val;
				if(key) *key=&record_addr->leaf.leaf[0].key;
				path.pathway[path_len].idx=0;
				return 1;
			}
			//branch
			path.pathway[path_len].idx=-1;
			record_addr = record_addr->branch.left;
		}
		return -2;
	}
	int get_next(const Key_t **key,Val_t** old_data,treepath& path) { //get next item after doing get_first(), find(), find_next() or get_next()
		//'path' must be saved from first get_first() or find() call.
		//returns 1-found,0-not found,-1 - invalid path, -2 - error to large depth, -3 - error impossible condition (in release mode only, in debug will assert)
		if(path.pathlen<0) return 0; //previous search ended or tree is empty
		int depth,idx;
		treeblock* p;

		depth=path.pathlen;
		p=path.pathway[depth].offset;
		if(p->isleaf!=1) {
			assert(false);
			return -3; //must be at leaf
		}

		idx=path.pathway[depth].idx+1; //increment idx to get correct index of next item

		//check if next item is in the same leaf
		if(idx<p->num) {
			if(sizeof(Val_t)>0 && old_data) *old_data=&p->leaf.leaf[idx].val;
			if(key) *key=&p->leaf.leaf[idx].key;
			path.pathway[depth].idx=idx;
			return 1;
		}
		//go up minimal until we find branch with next available item
		do {
			if(depth<=0) { //subtree has only leaf level or previous leaf was the last leaf of tree, get first item of next subtree
				return get_first(key, old_data, path, path.root_idx+1);
			}
			depth--;
			p=path.pathway[depth].offset;
			idx=path.pathway[depth].idx+1; //increment idx to check next branch item
		} while(idx>=p->num); //check that key can be found in current branch
		//here we found branch which can have key

		//continue regular search algorithm
		path.pathway[depth].idx=idx;
		p = p->branch.data[idx].offset;

		for(depth++;depth<int(MAX_TREE_DEPTH);depth++) {
			path.pathway[depth].offset=p;
			if(p->isleaf==1) { //leaf level, the deepest possible
				path.pathlen=depth;
				if(sizeof(Val_t)>0 && old_data) *old_data=&p->leaf.leaf[0].val;
				if(key) *key=&p->leaf.leaf[0].key;
				path.pathway[depth].idx=0;
				return 1;
			}
			//branch
			path.pathway[depth].idx=-1;
			p = p->branch.left;
		}
		return -2;
	}

	int find(Key_t const &key,Val_t** old_data,treepath* path=NULL,uint32_t *treeidx=NULL) { //treeidx - specific parameter for find_mult realization, selects specific root not accounting key
		//looks for key. if found and sizeof(Val_t)>0 and old_data!=NULL then saves pointer to data to old_data
		//returns 1-found,0-not found,-2 - error to large depth;
		//path.pathway contains pairs of numbers: [block offset, record offset inside the block(signed, equal -1 if new elem should be inserted before all)]
		//path.pathlen is index of last valid element in pathway
		if(!path) {
			path=(treepath*)alloca(sizeof(treepath));
		}
		treeblock* record_addr;
		if(!treeidx) {
			path->root_idx=(key & (((1u << Bits)-1) << Bitsoffset))>>Bitsoffset;
			record_addr=roots[path->root_idx];
		} else {
			path->root_idx=*treeidx;
			record_addr=roots[*treeidx];
		}
		if(!record_addr) { //root was not created yet
			path->pathlen=-1;
			return 0;
		}
		//begin search in the tree
		for(uint32_t path_len=0;path_len<MAX_TREE_DEPTH;path_len++) {
			path->pathway[path_len].offset=record_addr;
			int idx;
			if(record_addr->isleaf==1) { //leaf level, the deepest possible
				path->pathlen=(int)path_len;
				if(find_key_inleaf(key,record_addr,idx)) {
					if(sizeof(Val_t)>0 && old_data) *old_data=&record_addr->leaf.leaf[idx].val;
					path->pathway[path_len].idx=idx;
					return 1;
				}
				path->pathway[path_len].idx=idx;
				return 0;
			}
			//branch
			find_key_inbranch(key,record_addr,idx);
			path->pathway[path_len].idx=idx;
			record_addr = idx<0 ? record_addr->branch.left : record_addr->branch.data[idx].offset;
		}
		path->pathlen=-1;
		return -2;
	}
	int find_next(Key_t const &key,Val_t** old_data,treepath* path) { //after doing find() (or another find_next()) tries to find next key which is greater than
		//previous. 'path' must be saved from first find() call.
		//keys must be sorted in the bounds of every subtree, so to use natural key sorting throughout whole tree, Highbits must be true
		//returns 1-found,0-not found,-1 - invalid path or provided key is <= than previous, -2 - error to large depth, -3 - error impossible condition
		if(!path) return -1;
		if(path->pathlen<0 || path->root_idx!=((key & (((1u << Bits)-1) << Bitsoffset))>>Bitsoffset)) //subtree changed, so do new search
			return find(key, old_data, path);
		int depth,idx;
		treeblock* p;

		depth=path->pathlen;
		p=path->pathway[depth].offset;
		if(p->isleaf!=1) return -3; //must be at leaf

		idx=path->pathway[depth].idx+1; //increment idx because previous search could be unsuccessful (so index can be -1). this also means that same key won't be found again

		//check if key can be found in the same leaf
		if(idx<p->num && !(p->leaf.leaf[p->num-1].key<key)) { //key can be found in current leaf only
			if(find_key_inleaf(key,p,idx,idx)) {
				if(sizeof(Val_t)>0 && old_data) *old_data=&p->leaf.leaf[idx].val;
				path->pathway[depth].idx=idx;
				return 1;
			}
			path->pathway[depth].idx=idx;
			return 0;
		}
		//go up minimal until we find branch with next available item
		do {
			if(depth<=0) { //subtree has only leaf level or previous leaf was the last leaf of tree
				//path must point to the end of current leaf
				path->pathway[depth].idx=p->num-1;
				return 0;
			}
			depth--;
			p=path->pathway[depth].offset;
			idx=path->pathway[depth].idx+1; //increment idx to check next branch item
		} while(idx>=p->num || p->branch.data[p->num-1].key<key); //check that key can be found in current branch
		//here we found branch which can have key

		//continue regular search algorithm
		//first iteration is always over branch and must use special search function, so make it separetely
		find_key_inbranch(key,p,idx,idx);
		if(idx<0) return -1;
		path->pathway[depth].idx=idx;
		p = p->branch.data[idx].offset;

		for(depth++;depth<int(MAX_TREE_DEPTH);depth++) {
			path->pathway[depth].offset=p;
			if(p->isleaf==1) { //leaf level, the deepest possible
				path->pathlen=depth;
				if(find_key_inleaf(key,p,idx)) {
					if(sizeof(Val_t)>0 && old_data) *old_data=&p->leaf.leaf[idx].val;
					path->pathway[depth].idx=idx;
					return 1;
				}
				path->pathway[depth].idx=idx;
				return 0;
			}
			//branch
			find_key_inbranch(key,p,idx);
			path->pathway[depth].idx=idx;
			p = idx<0 ? p->branch.left : p->branch.data[idx].offset;
		}
		path->pathlen=-1;
		return -2;
	}
	int64_t find_mult(Key_t const &key_from,Key_t const &key_to,leafitem**buf, int64_t numbuf, bool isotropic=false) {//finds all records starting from key_from 
	//(including) up to key_to (not including). not more than numbuf leafitem structs will be saved in buf.
	//if isotropic is true then all keys between [key_from; key_to) are assumed to have the same hash (key & mask) which is calculated from key_from. This can
	//greatly speedup this function if mask is wide
	//returns total number of records (can be larger than numbuf) or  -2 - error too large depth, -3 - error impossible condition
		treepath path;
		if(!buf) numbuf=0;
		int64_t n=0;
		int depth,idx;
		treeblock* p;
		uint32_t i,lasti;
		if(isotropic) {
			i=(key_from & (((1u << Bits)-1) << Bitsoffset)) >> Bitsoffset;
			lasti=i;
		} else if(Highbits) {
			i=(key_from & (((1u << Bits)-1) << Bitsoffset)) >> Bitsoffset;
			lasti=(key_to & (((1u << Bits)-1) << Bitsoffset)) >> Bitsoffset;
		} else {
			i=0;
			lasti=(1u << Bits)-1;
		}
		for(;i<=lasti;i++) { //loop through all or particular root
			int rval=find(key_from,NULL,&path,&i);
			if(rval<0) return rval;
			depth=path.pathlen;
			if(depth<0) continue;
			if(path.pathway[depth].offset->isleaf!=1) return -3; //must be at leaf
			if(rval==0) path.pathway[depth].idx++; //when not found, increase idx
			idx=path.pathway[depth].idx;
			p=path.pathway[depth].offset;
			do {
				while(idx<p->num && p->leaf.leaf[idx].key<key_to) {
					if(n<numbuf) buf[n]=&p->leaf.leaf[idx];
					n++;
					idx++;
				}
				if(idx<p->num) break; //some key >= key_to was found
				//move to the next leaf
				if(depth<=0) break; //no parent branches
				//find deepest parent branch where idx is not at the end
				depth--;
				while(depth>=0) {
					if(path.pathway[depth].idx<path.pathway[depth].offset->num-1) break;
					depth--;
				}
				if(depth<0) break; //all idx's are at the end
				path.pathway[depth].idx++; //go to next leaf or branch
				//move down until leaf level is found
				p=path.pathway[depth].offset->branch.data[path.pathway[depth].idx].offset; //next child
				depth++;
				do {
					if(p->isleaf==1) {idx=0;break;} //don't update pathway for leaves
					path.pathway[depth].offset=p;
					path.pathway[depth].idx=-1;
					p=path.pathway[depth].offset->branch.left; //next child
					depth++;
				} while(depth<int(MAX_TREE_DEPTH));
			} while(n<int64_t(amount)); //just in case
		}
		return n;
	}
	int find_add(Key_t const &key,Val_t** prev_data,Val_t const &new_data, treepath *path=NULL) {
		//path if provided must be empty or from previous call to find/find_next/find_add for SMALLER key
		//returns 1-was appended (path will contains correct path of new item),0-was present (path will contain correct path of existing item),
		//-1 - invalid path or provided key is <= than previous,-2 - error too large depth,-3 - error impossible condition,-4 - error no memory;
		int split_idx,idx,rootidx,pathlen;
		treeblock* newblock,*newblock2;

		int rval;
		if(!path) {
			path=(treepath*)alloca(sizeof(treepath));
			path->pathlen=-1;
		}
		if(path->pathlen<0) {
			rval=find(key,prev_data,path);
		} else {
			rval=find_next(key,prev_data,path);
		}
		if(rval<0) return rval;
		if(rval==1) return 0; //found, prev_data will be filled with pointer to found data
		//wasn't found, try to insert
		treeblock* p;
		Key_t newparent;
		//fill free blocks struct to assure that memory is enough
		if(numfreeblocks<MAX_TREE_DEPTH+1) {
			uint32_t n=MAX_TREE_DEPTH+1+100;
			alloc_free_items<treeblock>(&mpool, n, bsize, freeblockshead);
			numfreeblocks+=n;
			if(numfreeblocks<MAX_TREE_DEPTH+1) return -4;
		}
		pathlen=path->pathlen;
		rootidx=(key & (((1u << Bits)-1) << Bitsoffset)) >> Bitsoffset;
		if(pathlen<0) { //root level must be created
			if(roots[rootidx]) return -3;
			roots[rootidx]=p=get_freeblock();
			p->isleaf=1; //init new root as leaf with one item
			p->num=1;
			p->leaf.leaf[0].key=key;
			if(sizeof(Val_t)>0) {
				p->leaf.leaf[0].val=new_data;
				if(prev_data) *prev_data=&p->leaf.leaf[0].val;
			}

			goto onsuccess;
		}
		p=path->pathway[pathlen].offset;
		if(p->isleaf!=1) return -3;
		//check that tree is not full
		if(pathlen>=int(MAX_TREE_DEPTH-1) && uint32_t(p->num)>=numperleaf) { //leaf is full, check all branches
			for(idx=pathlen-1;idx>=0;idx--) 
				if(uint32_t(path->pathway[idx].offset->num)<numperbranch) break;
			if(idx<0) return -2; //all branches were full
		}
		//we must be at the leaf
		idx=++(path->pathway[pathlen].idx); //increment idx (to get index of insertion, failed find() returns index of item AFTER WHICH new must be inserted)
		if(uint32_t(p->num)<numperleaf) { //simplest variant, just add new key
			if(idx < p->num) memmove(&p->leaf.leaf[idx+1],&p->leaf.leaf[idx],(p->num - idx)*sizeof(leafitem));
			p->leaf.leaf[idx].key=key;
			if(sizeof(Val_t)>0) {
				p->leaf.leaf[idx].val=new_data;
				if(prev_data) *prev_data=&p->leaf.leaf[idx].val;
			}
			p->num++;

			goto onsuccess;
		}
		//we have to split one leaf into two leaves
		split_idx=(numperleaf+1)>>1;
		newblock=get_freeblock();

		newblock->isleaf=1; //init new block as leaf
		newblock->num=p->num-(split_idx-1);
		int was_right; //remember to which side went item on previous step of tree. if right, then path index must be increased on parent level
		if(idx>=split_idx) { //new key will be the parent for new record or will be inserted into right leaf
			idx-=split_idx;
			if(idx!=0) memcpy(&newblock->leaf.leaf[0],&p->leaf.leaf[split_idx],idx*sizeof(leafitem));
			newblock->leaf.leaf[idx].key=key;
			if(sizeof(Val_t)>0) {
				newblock->leaf.leaf[idx].val=new_data;
				if(prev_data) *prev_data=&newblock->leaf.leaf[idx].val;
			}
			if(idx < newblock->num-1) memcpy(&newblock->leaf.leaf[idx+1],&p->leaf.leaf[split_idx+idx],(newblock->num-1-idx)*sizeof(leafitem)); //copy all but the new key, which was explicitly inserted
			path->pathway[pathlen].offset=newblock;
			path->pathway[pathlen].idx=idx;
			was_right=1;
		} else { //new key will be inserted into left leaf
			memcpy(&newblock->leaf.leaf[0],&p->leaf.leaf[split_idx-1],newblock->num*sizeof(leafitem)); //fill new block
			if(split_idx-1>idx) memmove(&p->leaf.leaf[idx+1],&p->leaf.leaf[idx],(split_idx-1-idx)*sizeof(leafitem)); //free space for new item in old block
			p->leaf.leaf[idx].key=key;
			if(sizeof(Val_t)>0) {
				p->leaf.leaf[idx].val=new_data;
				if(prev_data) *prev_data=&p->leaf.leaf[idx].val;
			}
			was_right=0;
		}
		p->num=split_idx;

		newparent=newblock->leaf.leaf[0].key; //new key which must be inserted to parent branch
		//here newparent and newblock must be set correctly
		for(pathlen--; pathlen>=0; pathlen--) {
			p=path->pathway[pathlen].offset;
			idx=path->pathway[pathlen].idx+1; //increment idx (new block will get bigger index)
			path->pathway[pathlen].idx+=was_right; //if on previous step new item was added to right (new) block, path index must be increased on this step
			if(uint32_t(p->num)<numperbranch) { //simplest variant, just add new branch
				if(idx < p->num) memmove(&p->branch.data[idx+1],&p->branch.data[idx],(p->num - idx)*sizeof(branchitem));
				p->branch.data[idx].key=newparent;
				p->branch.data[idx].offset=newblock;
				p->num++;

				goto onsuccess;
			}
			//we have to split one leaf into two leaves
			split_idx=(numperbranch+1)>>1;
			newblock2=get_freeblock();

			newblock2->isleaf=0; //init new block as branch
			newblock2->num=p->num-split_idx;
			if(idx==split_idx) { //new key will be the parent for new record
				newblock2->branch.left=newblock;
				memcpy(&newblock2->branch.data[0],&p->branch.data[split_idx],(newblock2->num)*sizeof(branchitem)); //copy all
//				newparent=newparent;  :)
				idx=-1;
			} else if(idx>split_idx) { //new key will be inserted into right leaf
				newblock2->branch.left=p->branch.data[split_idx].offset;
				idx-=split_idx+1;
				if(idx>0) memcpy(&newblock2->branch.data[0],&p->branch.data[split_idx+1],idx*sizeof(branchitem));
				newblock2->branch.data[idx].key=newparent;
				newblock2->branch.data[idx].offset=newblock;
				if(idx < newblock2->num-1) memcpy(&newblock2->branch.data[idx+1],&p->branch.data[split_idx+1+idx],(newblock2->num-1-idx)*sizeof(branchitem));
				newparent=p->branch.data[split_idx].key;
			} else { //new key will be inserted into left leaf
				newblock2->branch.left=p->branch.data[split_idx-1].offset;
				Key_t tmp=p->branch.data[split_idx-1].key;
				memcpy(&newblock2->branch.data[0],&p->branch.data[split_idx],(newblock2->num)*sizeof(branchitem)); //copy all
				if(idx < split_idx-1) memmove(&p->branch.data[idx+1],&p->branch.data[idx],(split_idx-1-idx)*sizeof(branchitem));
				p->branch.data[idx].key=newparent;
				p->branch.data[idx].offset=newblock;
				newparent=tmp;
			}
			if(path->pathway[pathlen].idx>=split_idx) {
				path->pathway[pathlen].offset=newblock2;
				path->pathway[pathlen].idx=idx;
				was_right=1;
			} else was_right=0;
			newblock=newblock2;
			p->num=split_idx;
		}
		//if we got here, new root must be created
		newblock2=get_freeblock();
		newblock2->isleaf=0; //init new block as branch
		newblock2->num=1;
		newblock2->branch.left=roots[rootidx];
		newblock2->branch.data[0].key=newparent;
		newblock2->branch.data[0].offset=newblock;
		roots[rootidx]=newblock2;
		//increase depth of saved path
		memmove(&path->pathway[1], &path->pathway[0], (path->pathlen+1)*sizeof(pathitem));
		path->pathway[0].offset=newblock2;
		path->pathway[0].idx=was_right-1;
		path->pathlen++;
onsuccess:
		amount++;
		return 1;
	}
	int remove(Key_t const &key,Val_t* prev_data=NULL, treepath *path=NULL, int (*checkfunc)(Val_t*, void*)=NULL, void *funcarg=NULL) { //optional prev_data is pointer to Val_t which will be filled with removed data
		//path if provided must be empty or from previous call to find/find_next/find_add for SAME or SMALLER key
		//optional checkfunc() can be used to prevent (by returning false) removing of item after check
		//
		//returns:
		// 1	was removed,
		// 0	was not found,
		// -1	invalid path or provided key is < than previous,
		// -2	error too large depth,
		// -3	error impossible condition,
		// -5	checkfunc provided and returned false;
		int idx, rval;
		bool updatekey,delkey;
		Val_t* prevdata;
		treeblock* p;
		int pathlen;

		if(!path) {
			path=(treepath*)alloca(sizeof(treepath));
			path->pathlen=-1;
		}
		if(path->pathlen<0) {
			rval=find(key,&prevdata,path);
		} else {
			pathlen=path->pathlen;
			p=path->pathway[pathlen].offset;
			idx=path->pathway[pathlen].idx;
			if(p->isleaf==1 && p->num>0 && idx>=0 && p->leaf.leaf[idx].key==key) goto skip_find; //path is exactly at key
			rval=find_next(key,&prevdata,path);
		}
		if(rval<0) return rval;
		if(rval==0) return 0; //not found
		//was found, remove
		pathlen=path->pathlen;
		if(pathlen<0) return -3; //if something found, pathlen must exist
		p=path->pathway[pathlen].offset;
		if(p->isleaf!=1 || p->num==0) return -3;
		idx=path->pathway[pathlen].idx;
skip_find:
		if(sizeof(Val_t)>0 && prev_data) *prev_data=*prevdata;
		if(checkfunc && !checkfunc(prevdata, funcarg)) return -5;
		Key_t newkey=0;
		//we must be at the leaf
		if(idx < p->num-1) memmove(&p->leaf.leaf[idx],&p->leaf.leaf[idx+1],(p->num - idx - 1)*sizeof(leafitem));
		p->num--;
		delkey = p->num==0; //have to delete key on lower level
		if(idx==0 && !delkey) {
			updatekey = true; //have to update key on lower level
			newkey=p->leaf.leaf[0].key;
		} else updatekey=false;
		bool invalidate_path=false; //flag that path must be invalidated. we do not update it correctly if some level is removed (may be TODO like for find_add)
		if(delkey) {
			invalidate_path=true;
			if(pathlen==0) roots[(key & (((1u << Bits)-1) << Bitsoffset)) >> Bitsoffset]=NULL; //root must be freed
			return_freeblock(p);
		} else {
			//decrement index in path for ability to use find_next/get_next/remove successfully (or they will skip next key)
			path->pathway[pathlen].idx--;
		}
		//update branches if necessary
		for(pathlen--; pathlen>=0 && (delkey || updatekey); pathlen--) {
			p=path->pathway[pathlen].offset;
			idx=path->pathway[pathlen].idx;
			if(delkey) {
				if(idx==-1) { //left branch
					updatekey=true;
					newkey=p->branch.data[0].key;
					p->branch.left=p->branch.data[0].offset;
					if(p->num>1) memmove(&p->branch.data[0],&p->branch.data[1],(p->num - 1)*sizeof(branchitem));
				} else {
					if(idx < p->num-1) memmove(&p->branch.data[idx],&p->branch.data[idx+1],(p->num - idx - 1)*sizeof(branchitem));
				}
				p->num--;
				if(p->num==0) { //this branch must be removed and parent attached to left child of this branch
					if(pathlen>0) { //there is parent
						if(path->pathway[pathlen-1].idx>=0) {
							path->pathway[pathlen-1].offset->branch.data[path->pathway[pathlen-1].idx].offset=p->branch.left;
							if(updatekey) path->pathway[pathlen-1].offset->branch.data[path->pathway[pathlen-1].idx].key=newkey;
							updatekey=false;
						} else {
							path->pathway[pathlen-1].offset->branch.left=p->branch.left;
						}
					} else { //root elem must be replaced
						roots[(key & (((1u << Bits)-1) << Bitsoffset)) >> Bitsoffset] = p->branch.left;
						updatekey=false;
					}
					return_freeblock(p);
				}
				else if(pathlen>0 && uint32_t(p->num)+uint32_t(path->pathway[pathlen-1].offset->num)<=numperbranch-2) { //this branch can be moved up to optimize tree tructure
					treeblock* const parent_p=path->pathway[pathlen-1].offset;
					const int parent_idx=path->pathway[pathlen-1].idx;
					if(parent_idx>=0) {
						parent_p->branch.data[parent_idx].offset=p->branch.left;
						if(updatekey) parent_p->branch.data[parent_idx].key=newkey;
						updatekey=false;
					} else {
						parent_p->branch.left=p->branch.left;
					}
					//extend space in parent branch for additional p->num items
					memmove(&parent_p->branch.data[parent_idx+1+p->num],&parent_p->branch.data[parent_idx+1],(parent_p->num-(parent_idx+1))*sizeof(branchitem));
					//move data from current branch
					memmove(&parent_p->branch.data[parent_idx+1],&p->branch.data[0],p->num*sizeof(branchitem));
					parent_p->num+=p->num;

					return_freeblock(p);
				}

				delkey=false;
				continue;
			}
			//updatekey is true
			if(idx>=0) {
				p->branch.data[idx].key=newkey;
				break;
			}
			//else go up
		}
		if(invalidate_path) path->pathlen=-1;

		amount--;
		return 1;
	}
	void removeall(void) {
		if(!roots) return;
		for(uint32_t i=0;i<(1u << Bits);i++) {
			if(roots[i]) {
				delbranch_(roots[i]);
				roots[i]=NULL;
			}
		}
		amount=0;
	}
	int print(bool astree,FILE* printfd,void (*printfunc)(FILE* fd,Key_t const *key,Val_t const* val),leafitem**vals=NULL,uint64_t *pnumdata=NULL) { //if astree true, tree will be printed as tree structure, otherwize just keys and datas for each hash index. 
//if vals specified, it will be filled with pointer to malloced memory with key-data pairs
//printfunc gets FD to make fprintf, optional pointers to key and valus (eigher can be absent)
//returns 0-ok,-1-error not inited or no printfunc,-4 - error no memory;
		if(!roots || !printfunc) return -1;
		if(vals) {
			*vals=(leafitem*)malloc(amount*sizeof(leafitem));
			if(!*vals) return -4;
		}
		if(pnumdata) *pnumdata=amount;
		uint64_t voff=0;
		for(uint32_t i=0;i<(1u << Bits);i++) {
			if(astree && printfd) fprintf(printfd,"Tree %u:\n",i);
			if(roots[i]) print_(roots[i],0,astree,printfd,printfunc,vals ? *vals : NULL,voff);
			if(printfd && astree) fprintf(printfd,"\n");
		}
		return 0;
	}
private:
	void delbranch_(treeblock* off) {
		if(off->isleaf==0) {
			delbranch_(off->branch.left);
			for(int i=0;i<off->num;i++)
				delbranch_(off->branch.data[i].offset);
		}
		return_freeblock(off);
	}
	void print_(treeblock* off,int level,bool astree,FILE* printfd,void (*printfunc)(FILE* fd,Key_t const *key,Val_t const* val),leafitem*vals,uint64_t &voff) {
		if(off->isleaf) {
			if(astree && printfd)fprintf(printfd,"%*sDATA(%u):",level*4,"",(uint32_t)off->num);
			for(int i=0;i<off->num;i++) {
				if(printfd) printfunc(printfd,&off->leaf.leaf[i].key,&off->leaf.leaf[i].val);
				if(vals) {
					vals[voff]=off->leaf.leaf[i];
					voff++;
				}
			}
			if(astree && printfd)fprintf(printfd,"\n");
			return;
		}
		if(astree && printfd) fprintf(printfd,"%*sleft\n",level*4,"");
		print_(off->branch.left,level+1,astree,printfd,printfunc,vals,voff);
		for(int i=0;i<off->num;i++) {
			if(astree && printfd) {fprintf(printfd,"%*s",level*4,"");printfunc(printfd,&off->branch.data[i].key,NULL);fprintf(printfd,"\n");}
			print_(off->branch.data[i].offset,level+1,astree,printfd,printfunc,vals,voff);
		}
	}
};

#endif


