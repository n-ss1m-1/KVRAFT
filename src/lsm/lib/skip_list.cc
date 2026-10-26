// skip_list.cc

#include<stdexcept>
#include<cassert>

#include "skip_list.h"


SkipList::Node::Node(const InternalKey& key, const std::string& value, int height)
    : key(key), value(value)
{
    if(height < 1 || kMaxHeight < height)  //! error怎么处理: 抛出异常
    {
        throw std::invalid_argument("SkipList Node height out of range");
    }
    next.resize(height, nullptr);
}

size_t SkipList::Node::ApproximateMemoryUsage() const noexcept
{
    return sizeof(Node)                         //! Node所有直接成员的 sizeof 之和 + 对齐 padding
         + key.user_key.size()                  //! 字符串实际的字节不包含在上面 需要独立计算
         + value.size()
         + next.size() * sizeof(Node*)
         + sizeof(std::unique_ptr<Node>);       // nodes_中的一个位置
}

bool SkipList::Iterator::Valid() const noexcept
{
    return current_ != nullptr;
}

const InternalKey& SkipList::Iterator::Key() const
{
    assert(Valid());            //! 承诺返回 只读引用对象，但是此处为非法迭代器，assert指定为调用方错误 -> 程序abort()
    return current_->key;
}

const std::string& SkipList::Iterator::Value() const
{
    assert(Valid());
    return current_->value;
}

void SkipList::Iterator::Next()
{
    assert(Valid());
    current_ = current_->next[0];
}

SkipList::Iterator::Iterator(const Node* node)
    :current_(node)
{
}

SkipList::SkipList()
{
    // 初始化虚拟头节点
    auto head = std::make_unique<Node>(InternalKey(),std::string(),kMaxHeight);
    nodes_.push_back(std::move(head));          //! unique_ptr只能移动 不能复制
    head_ = nodes_.back().get();            //上方移动之后 临时的head已为空 不再拥有Node

    //初始化随机种子
    std::random_device rd;
    random_.seed(rd());
}

SkipList::SkipList(HeightGenerator generator):heightGenerator_(generator)
{
    auto head = std::make_unique<Node>(InternalKey(),std::string(),kMaxHeight);
    nodes_.push_back(std::move(head));
    head_ = nodes_.back().get();

    std::random_device rd;
    random_.seed(rd());
}

bool SkipList::Insert(const InternalKey& key, const std::string& value)
{
    // 1. 校验 sequence 非零
    if(key.sequence == 0)
    {
        throw std::invalid_argument("InternalKey sequence cannot be zero");
    }

    // 2. 调用FindGreaterOrEqual() 建立predecessors(前驱)
    std::array<Node*,kMaxHeight> predecessors{};             //! {}初始化为nullptr
    Node* candidateNode = FindGreaterOrEqual(key, &predecessors);

    // 3. 候选节点非空 && Compare==0  ->  重复的节点
    if(candidateNode && comparator_.Compare(key, candidateNode->key)==0) return false;

    // 4. 调用RandomHeight() 获取并检查高度
    int height = RandomHeight();

    // 5. 创建Node 添加到nodes_
    nodes_.push_back(std::make_unique<Node>(key,value,height));

    // 6. 插入SkipList
    Node* newNode = nodes_.back().get();
    for(int level=0;level<height;level++)
    {
        newNode->next[level] = predecessors[level]->next[level];
        predecessors[level]->next[level] = newNode;
    }

    // 7. 更新估计的内存使用量
    approximateMemoryUsage_ += newNode->ApproximateMemoryUsage();

    return true;
}

SkipList::Iterator SkipList::Seek(const InternalKey& key) const
{
    if(Empty()) return Iterator(nullptr);               //空表判断：非必须，findGreaterOrEqual()也能处理空表

    return Iterator(FindGreaterOrEqual(key,nullptr));   // 结果：四种情况 不保证一定为target
}

//空表：head_->next[0]==nullptr -> 无效迭代器
//非空：指向首个真实节点 -> 有效迭代器
SkipList::Iterator SkipList::Begin() const
{
    return Iterator(head_->next[0]);
}

size_t SkipList::Size() const noexcept
{
    return nodes_.size()-1;      //! 排除头结点
}

bool SkipList::Empty() const noexcept
{
    return Size()==0;            //! 排除头结点
}

size_t SkipList::ApproximateMemoryUsage() const noexcept
{
    return approximateMemoryUsage_;
}

// Node 定义在 SkipList 的 private 块 -> 声明方法为 SkipList::Node
SkipList::Node* SkipList::FindGreaterOrEqual(const InternalKey& target, std::array<Node*, kMaxHeight>* predecessors) const
{
    Node* current = head_;

    // 对于Insert 每层都需要找前继节点
    for(int level=kMaxHeight-1;level>=0;level--)
    {
        // Compare返回true：继续往后找
        while(current->next[level] && comparator_(current->next[level]->key, target))
        {
            current = current->next[level];
        }
        if(predecessors) (*predecessors)[level] = current;      //! 注意需要判断是否为空(仅Insert需要使用，Seek不用)
    }

    return current->next[0];        //! 对于Seek: 返回下一个 大于等于target 的记录   1.目标节点  2.同一user_key的更旧版本  3.下一个user_key  4.nullptr
}

int SkipList::RandomHeight()
{
    int height = 1;
    if(heightGenerator_)
    {
        height = heightGenerator_();
        if(height < 1 || kMaxHeight < height) throw std::invalid_argument("generated skip list height out of range");
        return height;
    }

    while(height < kMaxHeight && random_() % 4 == 0)
    {
        height++;
    }

    return height;
}




/*!▲
嵌套类 类外定义成员函数时：

1. 传统返回类型位于限定函数名之前，嵌套类型通常需完整限定：
   SkipList::Node* SkipList::FindGreaterOrEqual(...)
   SkipList::Iterator SkipList::Begin() const

2. 限定函数名之后的参数列表、成员初始化列表和函数体，
   可以查找所属类及外层类中的名字：
   SkipList::Iterator::Iterator(const Node* node)
       : current_(node) {}

3. 嵌套类能访问外层 private 成员，但没有外层对象的隐式 this。

4. 外层类不能自动访问嵌套类的 private 成员。
   Iterator 通过 friend class SkipList 授权外层类调用私有构造函数。
*/
