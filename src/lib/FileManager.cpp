#include "FileManager.h"
#include "DummyKernel.h"
#include "Utility.h"
#include <cstdio>

/*==========================class FileManager===============================*/
void FileManager::Initialize()
{
    this->m_FileSystem = &DummyKernel::Instance().GetFileSystem();

    this->m_InodeTable = &g_InodeTable;
    this->m_OpenFileTable = &g_OpenFileTable;

    this->m_InodeTable->Initialize();
}

/*
 * 功能：打开文件
 * 效果：建立打开文件结构，内存i节点开锁 、i_count 为正数（i_count ++）
 * */
int FileManager::Open(char* path, int mode)
{
    Inode* pInode;
    User& u = DummyKernel::Instance().GetUser();

    u.u_dirp = path;
    pInode = this->NameI(FileManager::NextChar, FileManager::OPEN);	/* 0 = Open, not create */
    /* 没有找到相应的Inode */
    if ( NULL == pInode )
    {
        return -1;
    }
    this->Open1(pInode, mode, 0);
    return u.u_eax_ar0;
}

/*
 * 功能：创建一个新的文件
 * 效果：建立打开文件结构，内存i节点开锁 、i_count 为正数（应该是 1）
 * */
int FileManager::Creat(char* path, unsigned int mode)
{
    Inode* pInode;
    User& u = DummyKernel::Instance().GetUser();
    unsigned int newACCMode = mode & (Inode::IRWXU|Inode::IRWXG|Inode::IRWXO);
    
    u.u_dirp = path;
    /* 搜索目录的模式为1，表示创建；若父目录不可写，出错返回 */
    pInode = this->NameI(FileManager::NextChar, FileManager::CREATE);
    /* 没有找到相应的Inode，或NameI出错 */
    if ( NULL == pInode )
    {
        if(u.u_error)
            return -1;
        /* 创建Inode */
        pInode = this->MakNode( newACCMode & (~Inode::ISVTX) );
        /* 创建失败 */
        if ( NULL == pInode )
        {
            return -1;
        }

        /* 
         * 如果所希望的名字不存在，使用参数trf = 2来调用open1()。
         * 不需要进行权限检查，因为刚刚建立的文件的权限和传入参数mode
         * 所表示的权限内容是一样的。
         */
        this->Open1(pInode, File::FWRITE, 2);
        return u.u_eax_ar0;
    }
    else
    {
        /* 如果NameI()搜索到已经存在要创建的文件，则清空该文件（用算法ITrunc()）。UID没有改变
         * 原来UNIX的设计是这样：文件看上去就像新建的文件一样。然而，新文件所有者和许可权方式没变。
         * 也就是说creat指定的RWX比特无效。
         * 邓蓉认为这是不合理的，应该改变。
         * 现在的实现：creat指定的RWX比特有效 */
        this->Open1(pInode, File::FWRITE, 1);
        pInode->i_mode |= newACCMode;
        return u.u_eax_ar0;
    }
}

/* 
 * trf == 0由open调用
 * trf == 1由creat调用，creat文件的时候搜索到同文件名的文件
 * trf == 2由creat调用，creat文件的时候未搜索到同文件名的文件，这是文件创建时更一般的情况
 * mode参数：打开文件模式，表示文件操作是 读、写还是读写
 */
void FileManager::Open1(Inode* pInode, int mode, int trf)
{
    User& u = DummyKernel::Instance().GetUser();

    /* 
     * 对所希望的文件已存在的情况下，即trf == 0或trf == 1进行权限检查
     * 如果所希望的名字不存在，即trf == 2，不需要进行权限检查，因为刚建立
     * 的文件的权限和传入的参数mode的所表示的权限内容是一样的。
     */
    if (trf != 2)
    {
        if ( mode & File::FWRITE )
        {
            /* 系统调用去写目录文件是不允许的 */
            if ( (pInode->i_mode & Inode::IFMT) == Inode::IFDIR )
            {
                u.u_error = 1;
                printf("Unable to write directory @FileManager::Open1\n");
            }
        }
    }

    if ( u.u_error )
    {
        this->m_InodeTable->IPut(pInode);
        return;
    }

    /* 在creat文件的时候搜索到同文件名的文件，释放该文件所占据的所有盘块 */
    if ( 1 == trf )
    {
        pInode->ITrunc();
    }

    /* 解锁inode! 
     * 线性目录搜索涉及大量的磁盘读写操作，期间进程会入睡。
     * 因此，进程必须上锁操作涉及的i节点。这就是NameI中执行的IGet上锁操作。
     * 行至此，后续不再有可能会引起进程切换的操作，可以解锁i节点。
     */
    pInode->Prele();

    /* 分配打开文件控制块File结构 */
    File* pFile = this->m_OpenFileTable->FAlloc();
    if ( NULL == pFile )
    {
        this->m_InodeTable->IPut(pInode);
        return;
    }
    /* 设置打开文件方式，建立File结构和内存Inode的勾连关系 */
    pFile->f_flag = mode & (File::FREAD | File::FWRITE);
    pFile->f_inode = pInode;

    /* 为打开或者创建文件的各种资源都已成功分配，函数返回 */
    if ( u.u_error == 0 )
    {
        return;
    }
    else	/* 如果出错则释放资源 */
    {
        /* 释放打开文件描述符 */
        int fd = u.u_eax_ar0;
        if(fd != -1)
        {
            u.u_ofiles.SetF(fd, NULL);
            /* 递减File结构和Inode的引用计数 ,File结构没有锁 f_count为0就是释放File结构了*/
            pFile->f_count--;
        }
        this->m_InodeTable->IPut(pInode);
    }
}


void FileManager::Close(int fd)
{
    User& u = DummyKernel::Instance().GetUser();

    /* 获取打开文件控制块File结构 */
    File* pFile = u.u_ofiles.GetF(fd);
    if ( NULL == pFile )
    {
        return;
    }

    /* 释放打开文件描述符fd，递减File结构引用计数 */
    u.u_ofiles.SetF(fd, NULL);
    this->m_OpenFileTable->CloseF(pFile);
}

void FileManager::Seek(int fd, int offset, int mode)
{
    File* pFile;
    User& u = DummyKernel::Instance().GetUser();

    pFile = u.u_ofiles.GetF(fd);
    if ( NULL == pFile )
    {
        return;  /* 若FILE不存在，GetF有设出错码 */
    }

    /* 管道文件不允许seek */
    //if ( pFile->f_flag & File::FPIPE )
    //{
        //u.u_error = 1;
        //return;
    //}

    /* 如果u.u_arg[2]在3 ~ 5之间，那么长度单位由字节变为512字节 */
    if ( mode > 2 )
    {
        offset = offset << 9;
        mode -= 3;
    }

    switch ( mode )
    {
        /* 读写位置设置为offset */
        case 0:
            pFile->f_offset = offset;
            break;
            /* 读写位置加offset(可正可负) */
        case 1:
            pFile->f_offset += offset;
            break;
            /* 读写位置调整为文件长度加offset */
        case 2:
            pFile->f_offset = pFile->f_inode->i_size + offset;
            break;
    }
}

int FileManager::Read(int fd, char* buffer, int count)
{
    /* 直接调用Rdwr()函数即可 */
    this->Rdwr(fd, buffer, count, File::FREAD);
    return DummyKernel::Instance().GetUser().u_eax_ar0;
}

int FileManager::Write(int fd, char* buffer, int count)
{
    /* 直接调用Rdwr()函数即可 */
    this->Rdwr(fd, buffer, count, File::FWRITE);
    return DummyKernel::Instance().GetUser().u_eax_ar0;
}

void FileManager::Rdwr(int fd, char* buffer, int count, enum File::FileFlags mode )
{
    File* pFile;
    User& u = DummyKernel::Instance().GetUser();

    /* 根据Read()/Write()的系统调用参数fd获取打开文件控制块结构 */
    pFile = u.u_ofiles.GetF(fd);	/* fd */
    if ( NULL == pFile )
    {
        /* 不存在该打开文件，GetF已经设置过出错码，所以这里不需要再设置了 */
        /*	u.u_error = User::EBADF;	*/
        u.u_error = 1;
        return;
    }


    /* 读写的模式不正确 */
//    if ( (pFile->f_flag & mode) == 0 )
//    {
//        u.u_error = 1;
//        return;
//    }

    u.u_IOParam.m_Base = (unsigned char *)buffer;	/* 目标缓冲区首址 */
    u.u_IOParam.m_Count = count;		/* 要求读/写的字节数 */
    //u.u_segflg = 0;		[> User Space I/O，读入的内容要送数据段或用户栈段 <]

    /* 普通文件读写 ，或读写特殊文件。对文件实施互斥访问，互斥的粒度：每次系统调用。
       为此Inode类需要增加两个方法：NFlock()、NFrele()。
       这不是V6的设计。read、write系统调用对内存i节点上锁是为了给实施IO的进程提供一致的文件视图。*/
    {
        /* 设置文件起始读位置 */
        u.u_IOParam.m_Offset = pFile->f_offset;
        if ( File::FREAD == mode )
        {
            pFile->f_inode->ReadI();
        }
        else
        {
            pFile->f_inode->WriteI();
        }

        /* 根据读写字数，移动文件读写偏移指针 */
        pFile->f_offset += (count - u.u_IOParam.m_Count);
    }

    /* 返回实际读写的字节数，修改存放系统调用返回值的核心栈单元 */
    u.u_eax_ar0 = count - u.u_IOParam.m_Count;
}

void FileManager::Ls(char* path) {
    Inode* pInode;
    User& u = DummyKernel::Instance().GetUser();
    u.u_dirp = path;
    
    int dirEntryCount = 0;
    BufferManager& bufMgr = DummyKernel::Instance().GetBufferManager();
    Buf* pBuf;
    
    pInode = this->NameI(FileManager::NextChar, FileManager::OPEN);
    if(NULL == pInode) {
        printf("[Path Not Found]\n");
        return;
    }


    /* 对于u.u_dbuf[]中的路径名分量，逐个搜寻匹配的目录项 */
    u.u_IOParam.m_Offset = 0;
    /* 设置为目录项个数 ，含空白的目录项*/
    u.u_IOParam.m_Count = pInode->i_size / (DirectoryEntry::DIRSIZ + 4);
    pBuf = NULL;
    while(true) {
        /* 对目录项已经搜索完毕 */
        if ( 0 == u.u_IOParam.m_Count )
        {
            if ( NULL != pBuf )
            {
                bufMgr.Brelse(pBuf);
            }

            printf("%d entries in all.\n", dirEntryCount);
            break;
        }

        /* 已读完目录文件的当前盘块，需要读入下一目录项数据盘块 */
        if ( 0 == u.u_IOParam.m_Offset % Inode::BLOCK_SIZE )
        {
            if ( NULL != pBuf )
            {
                bufMgr.Brelse(pBuf);
            }
            /* 计算要读的物理盘块号 */
            int phyBlkno = pInode->Bmap(u.u_IOParam.m_Offset / Inode::BLOCK_SIZE );
            pBuf = bufMgr.Bread(pInode->i_dev, phyBlkno );
        }

        /* 没有读完当前目录项盘块，则读取下一目录项至u.u_dent */
        int* src = (int *)(pBuf->b_addr + (u.u_IOParam.m_Offset % Inode::BLOCK_SIZE));
        Utility::DWordCopy( src, (int *)&u.u_dent, sizeof(DirectoryEntry)/sizeof(int) );

        u.u_IOParam.m_Offset += (DirectoryEntry::DIRSIZ + 4);
        u.u_IOParam.m_Count--;

        if ( 0 == u.u_dent.m_ino )
        {
            /* 跳过空闲目录项，继续搜索下一目录项 */
            continue;
        }

        dirEntryCount++;
        printf("%s\t\t%d\n", u.u_dent.m_name, u.u_dent.m_ino);
    }

    //pInode->Prele();
    this->m_InodeTable->IPut(pInode);
}

/* 返回NULL表示目录搜索失败，否则是根指针，指向文件的内存打开i节点 ，上锁的内存i节点  */
Inode* FileManager::NameI( char (*func)(), enum DirectorySearchMode mode )
{
    Inode* pInode;
    Buf* pBuf;
    char curchar;
    char* pChar;
    int freeEntryOffset;	/* 以创建文件模式搜索目录时，记录空闲目录项的偏移量 */
    User& u = DummyKernel::Instance().GetUser();
    BufferManager& bufMgr = DummyKernel::Instance().GetBufferManager();

    /* 
     * 如果该路径是'/'开头的，从根目录开始搜索，
     * 否则从进程当前工作目录开始搜索。
     */
    pInode = u.u_cdir;
    if ( '/' == (curchar = (*func)()) )
    {
        pInode = this->rootDirInode;
    }

    /* 检查该Inode是否正在被使用，以及保证在整个目录搜索过程中该Inode不被释放 */
    this->m_InodeTable->IGet(pInode->i_dev, pInode->i_number);

    /* 允许出现////a//b 这种路径 这种路径等价于/a/b */
    while ( '/' == curchar )
    {
        curchar = (*func)();
        //putchar(curchar);
    }
    /* 如果试图更改和删除当前目录文件则出错 */
    if ( '\0' == curchar && mode != FileManager::OPEN )
    {
        u.u_error = 1;
        goto out;
    }

    //printf("%c", curchar);
    /* 外层循环每次处理pathname中一段路径分量 */
    while (true)
    {
        /* 如果出错则释放当前搜索到的目录文件Inode，并退出 */
        if ( u.u_error != 0 )
        {
            break;	/* goto out; */
        }

        /* 整个路径搜索完毕，返回相应Inode指针。目录搜索成功返回。 */
        if ( '\0' == curchar )
        {
            return pInode;
        }

        /* 如果要进行搜索的不是目录文件，释放相关Inode资源则退出 */
        if ( (pInode->i_mode & Inode::IFMT) != Inode::IFDIR )
        {
            printf("Not a directory. @FileManager::NameI\n");
            u.u_error = 1;
            break;	/* goto out; */
        }

        /* 
         * 将Pathname中当前准备进行匹配的路径分量拷贝到u.u_dbuf[]中，
         * 便于和目录项进行比较。
         */
        pChar = &(u.u_dbuf[0]);
        while ( '/' != curchar && '\0' != curchar && u.u_error == 0 )
        {
            if ( pChar < &(u.u_dbuf[DirectoryEntry::DIRSIZ]) )
            {
                *pChar = curchar;
                pChar++;
            }
            curchar = (*func)();
        }
        /* 将u_dbuf剩余的部分填充为'\0' */
        while ( pChar < &(u.u_dbuf[DirectoryEntry::DIRSIZ]) )
        {
            *pChar = '\0';
            pChar++;
        }

        /* 允许出现////a//b 这种路径 这种路径等价于/a/b */
        while ( '/' == curchar )
        {
            curchar = (*func)();
        }

        if ( u.u_error != 0 )
        {
            break; /* goto out; */
        }

        /* 内层循环部分对于u.u_dbuf[]中的路径名分量，逐个搜寻匹配的目录项 */
        u.u_IOParam.m_Offset = 0;
        /* 设置为目录项个数 ，含空白的目录项*/
        u.u_IOParam.m_Count = pInode->i_size / (DirectoryEntry::DIRSIZ + 4);
        freeEntryOffset = 0;
        pBuf = NULL;

        while (true)
        {
            /* 对目录项已经搜索完毕 */
            if ( 0 == u.u_IOParam.m_Count )
            {
                if ( NULL != pBuf )
                {
                    bufMgr.Brelse(pBuf);
                }
                /* 如果是创建新文件 */
                if ( FileManager::CREATE == mode && curchar == '\0' )
                {
                    /* 将父目录Inode指针保存起来，以后写目录项WriteDir()函数会用到 */
                    u.u_pdir = pInode;

                    if ( freeEntryOffset )	/* 此变量存放了空闲目录项位于目录文件中的偏移量 */
                    {
                        /* 将空闲目录项偏移量存入u区中，写目录项WriteDir()会用到 */
                        u.u_IOParam.m_Offset = freeEntryOffset - (DirectoryEntry::DIRSIZ + 4);
                    }
                    else  /*问题：为何if分支没有置IUPD标志？  这是因为文件的长度没有变呀*/
                    {
                        pInode->i_flag |= Inode::IUPD;
                    }
                    /* 找到可以写入的空闲目录项位置，NameI()函数返回 */
                    return NULL;
                }

                /* 目录项搜索完毕而没有找到匹配项，释放相关Inode资源，并推出 */
                printf("Not found. @FileManager::NameI\n");
                u.u_error = 1;
                goto out;
            }

            /* 已读完目录文件的当前盘块，需要读入下一目录项数据盘块 */
            if ( 0 == u.u_IOParam.m_Offset % Inode::BLOCK_SIZE )
            {
                if ( NULL != pBuf )
                {
                    bufMgr.Brelse(pBuf);
                }
                /* 计算要读的物理盘块号 */
                int phyBlkno = pInode->Bmap(u.u_IOParam.m_Offset / Inode::BLOCK_SIZE );
                pBuf = bufMgr.Bread(pInode->i_dev, phyBlkno );
            }

            /* 没有读完当前目录项盘块，则读取下一目录项至u.u_dent */
            int* src = (int *)(pBuf->b_addr + (u.u_IOParam.m_Offset % Inode::BLOCK_SIZE));
            Utility::DWordCopy( src, (int *)&u.u_dent, sizeof(DirectoryEntry)/sizeof(int) );

            u.u_IOParam.m_Offset += (DirectoryEntry::DIRSIZ + 4);
            u.u_IOParam.m_Count--;

            /* 如果是空闲目录项，记录该项位于目录文件中偏移量 */
            if ( 0 == u.u_dent.m_ino )
            {
                if ( 0 == freeEntryOffset )
                {
                    freeEntryOffset = u.u_IOParam.m_Offset;
                }
                /* 跳过空闲目录项，继续比较下一目录项 */
                continue;
            }

            int i;
            for ( i = 0; i < DirectoryEntry::DIRSIZ; i++ )
            {
                if ( u.u_dbuf[i] != u.u_dent.m_name[i] )
                {
                    break;	/* 匹配至某一字符不符，跳出for循环 */
                }
            }

            if( i < DirectoryEntry::DIRSIZ )
            {
                /* 不是要搜索的目录项，继续匹配下一目录项 */
                continue;
            }
            else
            {
                /* 目录项匹配成功，回到外层While(true)循环 */
                break;
            }
        }

        /* 
         * 从内层目录项匹配循环跳至此处，说明pathname中
         * 当前路径分量匹配成功了，还需匹配pathname中下一路径
         * 分量，直至遇到'\0'结束。
         */
        if ( NULL != pBuf )
        {
            bufMgr.Brelse(pBuf);
        }

        /* 如果是删除操作，则返回父目录Inode，而要删除文件的Inode号在u.u_dent.m_ino中 */
        if ( FileManager::DELETE == mode && '\0' == curchar )
        {
            return pInode;
        }

        /* 
         * 匹配目录项成功，则释放当前目录Inode，根据匹配成功的
         * 目录项m_ino字段获取相应下一级目录或文件的Inode。
         */
        short dev = pInode->i_dev;
        this->m_InodeTable->IPut(pInode);
        pInode = this->m_InodeTable->IGet(dev, u.u_dent.m_ino);
        /* 回到外层While(true)循环，继续匹配Pathname中下一路径分量 */

        if ( NULL == pInode )	/* 获取失败 */
        {
            return NULL;
        }
    }
out:
    this->m_InodeTable->IPut(pInode);
    return NULL;
}

char FileManager::NextChar()
{
    User& u = DummyKernel::Instance().GetUser();

    /* u.u_dirp指向pathname中的字符 */
    return *u.u_dirp++;
}

/* 由creat调用。
 * 为新创建的文件写新的i节点和新的目录项
 * 返回的pInode是上了锁的内存i节点，其中的i_count是 1。
 *
 * 在程序的最后会调用 WriteDir，在这里把属于自己的目录项写进父目录，修改父目录文件的i节点 、将其写回磁盘。
 *
 */
Inode* FileManager::MakNode( unsigned int mode )
{
    Inode* pInode;
    User& u = DummyKernel::Instance().GetUser();

    /* 分配一个空闲DiskInode，里面内容已全部清空 */
    pInode = this->m_FileSystem->IAlloc(u.u_pdir->i_dev);
    if( NULL ==	pInode )
    {
        return NULL;
    }

    pInode->i_flag |= (Inode::IACC | Inode::IUPD);
    pInode->i_mode = mode | Inode::IALLOC;
    pInode->i_nlink = 1;
    /* 将目录项写入u.u_dent，随后写入目录文件 */
    this->WriteDir(pInode);
    return pInode;
}

void FileManager::WriteDir( Inode* pInode )
{
    User& u = DummyKernel::Instance().GetUser();

    /* 设置目录项中Inode编号部分 */
    u.u_dent.m_ino = pInode->i_number;

    /* 设置目录项中pathname分量部分 */
    for ( int i = 0; i < DirectoryEntry::DIRSIZ; i++ )
    {
        u.u_dent.m_name[i] = u.u_dbuf[i];
    }

    u.u_IOParam.m_Count = DirectoryEntry::DIRSIZ + 4;
    u.u_IOParam.m_Base = (unsigned char *)&u.u_dent;
    //u.u_segflg = 1;

    /* 将目录项写入父目录文件 */
    u.u_pdir->WriteI();
    this->m_InodeTable->IPut(u.u_pdir);
}

void FileManager::SetCurDir(char* pathname)
{
    User& u = DummyKernel::Instance().GetUser();

    /* 路径不是从根目录'/'开始，则在现有u.u_curdir后面加上当前路径分量 */
    if ( pathname[0] != '/' )
    {
        int length = Utility::StringLength(u.u_curdir);
        if ( u.u_curdir[length - 1] != '/' )
        {
            u.u_curdir[length] = '/';
            length++;
        }
        Utility::StringCopy(pathname, u.u_curdir + length);
    }
    else	/* 如果是从根目录'/'开始，则取代原有工作目录 */
    {
        Utility::StringCopy(pathname, u.u_curdir);
    }
}



void FileManager::UnLink(char* path)
{
    Inode* pInode;
    Inode* pDeleteInode;
    User& u = DummyKernel::Instance().GetUser();

    u.u_dirp = path;
    pDeleteInode = this->NameI(FileManager::NextChar, FileManager::DELETE);
    if ( NULL == pDeleteInode )
    {
        return;
    }
    pDeleteInode->Prele();

    pInode = this->m_InodeTable->IGet(pDeleteInode->i_dev, u.u_dent.m_ino);
    if ( NULL == pInode )
    {
        printf("unlink -- iget");
    }
    /* 只有root可以unlink目录文件 */
    //if ( (pInode->i_mode & Inode::IFMT) == Inode::IFDIR && !u.SUser() )
    //{
        //this->m_InodeTable->IPut(pDeleteInode);
        //this->m_InodeTable->IPut(pInode);
        //return;
    //}
    /* 写入清零后的目录项 */
    u.u_IOParam.m_Offset -= (DirectoryEntry::DIRSIZ + 4);
    u.u_IOParam.m_Base = (unsigned char *)&u.u_dent;
    u.u_IOParam.m_Count = DirectoryEntry::DIRSIZ + 4;

    u.u_dent.m_ino = 0;
    pDeleteInode->WriteI();

    /* 修改inode项 */
    pInode->i_nlink--;
    pInode->i_flag |= Inode::IUPD;

    this->m_InodeTable->IPut(pDeleteInode);
    this->m_InodeTable->IPut(pInode);
}

bool FileManager::IsDirEmpty(char* path) {
    FileSystem& fileSys = DummyKernel::Instance().GetFileSystem();
    User& u = DummyKernel::Instance().GetUser();
    u.u_dirp = path;
    Inode* pInode = this->NameI(FileManager::NextChar, FileManager::OPEN);
    BufferManager& bufMgr = DummyKernel::Instance().GetBufferManager();
    int r = 0;

    
    if(pInode == NULL) {
        return false;
    }
    
    if((pInode->i_mode & Inode::IFMT)!=Inode::IFDIR ) {
        this->m_InodeTable->IPut(pInode);
        printf("Not a directory. \n");
        return false;
    }
    
    const int EMPTY_DIR_SIZE = 0;
    int dir_entry_count = 0;
    
    /* 内层循环部分对于u.u_dbuf[]中的路径名分量，逐个搜寻匹配的目录项 */
    u.u_IOParam.m_Offset = 0;
    /* 设置为目录项个数 ，含空白的目录项*/
    u.u_IOParam.m_Count = pInode->i_size / (DirectoryEntry::DIRSIZ + 4);
    int freeEntryOffset = 0;
    Buf* pBuf = NULL;
    
    while(true) {
        if( 0 == u.u_IOParam.m_Count) {
            if(NULL!=pBuf) {
                bufMgr.Brelse(pBuf);
            }
            this->m_InodeTable->IPut(pInode);
            return !r;
        }
        
        if ( 0 == u.u_IOParam.m_Offset % Inode::BLOCK_SIZE )
        {
            if ( NULL != pBuf )
            {
                bufMgr.Brelse(pBuf);
            }
            /* 计算要读的物理盘块号 */
            int phyBlkno = pInode->Bmap(u.u_IOParam.m_Offset / Inode::BLOCK_SIZE );
            pBuf = bufMgr.Bread(pInode->i_dev, phyBlkno );
        }
        int* src = (int *)(pBuf->b_addr + (u.u_IOParam.m_Offset % Inode::BLOCK_SIZE));
        Utility::DWordCopy( src, (int *)&u.u_dent, sizeof(DirectoryEntry)/sizeof(int) );
        
        u.u_IOParam.m_Offset += (DirectoryEntry::DIRSIZ + 4);
        u.u_IOParam.m_Count--;
        
        /* 如果是空闲目录项，记录该项位于目录文件中偏移量 */
        if ( 0 == u.u_dent.m_ino )
        {
            /* 跳过空闲目录项，继续比较下一目录项 */
            continue;
        }
        
        r++;
    }
}

/*==========================class DirectoryEntry===============================*/
DirectoryEntry::DirectoryEntry()
{
    this->m_ino = 0;
    this->m_name[0] = '\0';
}
