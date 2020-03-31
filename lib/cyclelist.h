/*************************
c���� ѭ�����нṹ�������ڱ����һ�����±ꡢ��ָ�룬��Ӧ�ñ������ṹ�壬��Ԫ���� sizeof(long);
֧�ֵ��̣߳������̷ֱ߳�push��pop�ĳ�������֧�ֶ��̲߳���push������pop������
Ҫ��list����Ϊ2�������ݣ�ȡ�����&(size-1) ���mod��

* ��ͷ���ӣ�head ָ���һ�����õ�Ԫ����β��ӣ�tailָ���һ�����е�Ԫ��
* Initʱ��������(tail + 1)mod size == head��ʵ���������Ϊ size - 1��
* �ӿ�ʱ�� head == tail��
* �����ж϶��зǿգ��򷵻�head��head���ƣ���ӣ��ж϶��в������򷵻�tail��tail���ơ�
*****************************/
#ifndef		__CYCLE_LIST_C_
#define		__CYCLE_LIST_C_

#define BITS_SIZE	sizeof(long)					//  �ֳ���

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
	long	*ele;		// ������Դ�ĵ�ַ�ռ�
	int		size;		// ��Դ���鳤��
	//int		FreeNum;	// �������鳤��
	int 	Head;		// ָ���һ��������Դ
	int		Tail;		// ָ���һ���ǿ�����Դ
}CycleList_t, CList_t;

/**********************
 ����ѭ�����е�ʵ�ʿռ䳤�ȣ� 
 ʵ�ʿռ�Ӧ���ǲ�С�� ���size����С 2�ݡ�
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

//  ��ȡѭ�����еĿ�����Դ����
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

