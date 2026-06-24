---
name: frontend-important-comments
description: Use when writing, editing, or reviewing frontend code. Requires comments to be plain, easy to understand, and focused on why the code is written this way instead of listing framework-specific rules or repeating what the code already says.
---

# Frontend Important Comments

写前端代码时，重要注释必须通俗易懂，并且说明“为什么”。

## 核心规则

注释是写给以后看代码的人看的，不是写给机器看的。

好的注释应该回答：

- 为什么这里要这样写？
- 这里有什么容易误改的地方？
- 如果改错了，会坏在哪里？
- 为什么不用看起来更简单的写法？

不要写成技术名词堆砌，也不要写成代码翻译。

## 应该怎么写

用普通人能听懂的话解释原因。

优先写短句。能一句话说清楚，就不要写一大段。

注释要贴近代码，放在真正需要解释的地方。

如果代码本身已经很清楚，不要硬加注释。

## 不要这样写

```ts
// 遍历数组
items.map(renderItem)
```

```ts
// 设置状态
setSelectedId(id)
```

这类注释只是把代码念了一遍，没有价值。

## 应该这样写

```ts
// 先按时间排序，避免后来的旧事件把新状态覆盖掉。
const orderedEvents = sortEvents(events)
```

```ts
// 这里保留未知字段，是为了旧版本数据也能在详情面板里看到。
const detail = keepUnknownFields(raw)
```

```ts
// 不在组件里直接改世界状态，避免多个面板看到的状态不一致。
world.apply(effect)
```

## 写完检查

每次写完前端代码，检查一遍：

1. 关键地方有没有说明为什么？
2. 注释是不是普通人能看懂？
3. 有没有只是在复述代码？
4. 注释会不会让以后的人更敢改代码？
5. 如果代码改了，注释是否也需要同步改？
