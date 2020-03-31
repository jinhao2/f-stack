/*************************
c代码 循环队列结构，队列内保存的一定是下标、或指针，不应该保存具体结构体，单元长度 sizeof(long);
支持单线程，两个线程分别push、pop的场景，不支持多线程并发push、并发pop操作。
要求list长度为2的整数幂，取余操作&(size-1) 替代mod。

* 队头出队，head 指向第一个可用单元；队尾入队，tail指向第一个空闲单元；
* Init时，队满，(tail + 1)mod size == head，实际入队数量为 size - 1；
* 队空时， head == tail；
* 出队判断队列非空，则返回head，head后移；入队，判断队列不满，则返回tail，tail后移。
*****************************/
#ifndef		__CYCLE_LIST_C_
#define		__CYCLE_LIST_C_

#define BITS_SIZE	sizeof(long)					//  字长度

/******Cal level size**/
static int cal_exp (unsigned long size)
{
	unsigned long i = 1;
	int j = 0;

	while(i < size) {
		i *= 2;
		j ++;
	}
	return (i);
}

typedef struct _list_pool_s
{
	long	*ele;		// 保存资源的地址空间
	int		size;		// 资源数组长度
	//int		FreeNum;	// 空闲数组长度
	int 	Head;		// 指向第一个空闲资源
	int		Tail;		// 指向第一个非空闲资源
}CycleList_t, CList_t;

/**********************
 返回循环队列的实际空间长度， 
 实际空间应该是不小于 入参size的最小 2幂。
**********************/
int CList_init(CList_t *p, int size)
{
	int i = 0;
	int len = 0;
	
	if (p==NULL || size<=0)
	{
		return -1;
	}
	len = cal_exp(size);
	//p->ele = (int*)malloc(sizeof(int)*size);
	posix_memalign((void**)&p->ele, BITS_SIZE, size*sizeof(long));
	if (p->ele == NULL)
		return -2;
	
	p->Head = p->Tail = 0;
	p->size = len;
	
	return p->size;
}

//  获取循环队列的可用资源数量
int CList_GetLen(CList_t *p)
{
	int head, tail;
	
	if(p==NULL)
		return -1;
	head = p->Head;
	tail = p->Tail;
	return  ((tail - head + p->size) & (p->size -1));
}

int CList_IsEmpty(CList_t *p)
{
	return  p->Head == p->Tail;
}

int CList_IsFull(CList_t *p)
{
	return  p->Head == ((p->Tail + 1) & (p->size -1)) ;
}

//return code: -1: faile;  >=0:OK.
int CList_Pop(CList_t *p)
{
	int head = 0;
	
	if(p==NULL)
		return -1;

	if ( !CList_IsEmpty(p))
	{
		head = p->Head;		
		p->Head = (p->Head + 1) & (p->size-1);
		return p->ele[head];
	}
	else
		return -1;
}

//id: the id of element to be push into list.
//return code: -1: faile;  >=0:OK.
int CList_Push(CList_t *p, long val)
{
	int tail = 0;
	
	if(p==NULL)
		return -1;
	if ( !CList_IsFull(p) )
	{
		tail = p->Tail;
		p->ele[tail] = val;
		p->Tail = (p->Tail + 1) & (p->size-1);
		
		return 0;
	}
	else
		return -1;
}

inline int CList_GetSize(CList_t *p)
{
	if(p==NULL)
		return -1;
	return p->size;
}

#endif				//  __CYCLE_LIST_C_

