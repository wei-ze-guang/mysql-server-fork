---
name: frontend-dense-apple-style
description: Use when writing, editing, or reviewing frontend UI in this repository. Requires Apple-like system fonts, small readable typography, minimal spacing, compact panels, content-first layouts, and stable component dimensions that avoid layout jitter.
---

# Frontend Dense Apple Style

本项目页面要像“高密度调试观察台”，不是营销页。空间要让给内容。

## 字体

优先使用接近 Apple 的系统字体：

```css
font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "SF Pro Display", "Segoe UI", sans-serif;
```

代码、id、event_name 使用清晰的等宽字体：

```css
font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
```

## 字号

默认字号尽量小，但必须能读清楚：

- 正文：12px-13px
- 次要说明：11px-12px
- 标题：18px-22px
- 代码、id、event_name：11px-12px

不要为了显得“高级”把标题放很大。这个项目内容多，大字会挤占有效空间。

## 间距

内外边距尽量小：

- 页面 padding：8px-16px
- 卡片 padding：8px-12px
- 区块 gap：8px-12px
- 列表项 padding：4px-8px

不要大面积留白。留白只用于分组和阅读，不用于装饰。

## 组件尺寸

组件要尺寸稳定，避免抖动。

写组件时要注意：

- 按钮、卡片、列表项、图节点要有稳定高度或明确的 min/max 约束。
- hover、选中、加载、聚焦状态不能让布局跳动。
- 动态文本要能换行、截断或横向滚动，不能把容器撑坏。
- 图节点、timeline item、detail panel 不要因为内容变化忽大忽小。
- loading、empty、error 状态也要占用合理空间，避免页面突然重排。

## 布局

优先使用紧凑的信息布局：

- 多列面板
- 小卡片
- 紧凑列表
- 可滚动详情区
- 固定工具栏

不要使用：

- 大 hero
- 大标题
- 大插图
- 大渐变背景
- 装饰性大留白

## 检查清单

完成 UI 前检查：

1. 字体是否接近 Apple 系统字体？
2. 字号是否足够小但仍然清楚？
3. padding 和 gap 是否把空间留给内容？
4. 长 id、event_name、JSON 是否不会撑坏布局？
5. hover、选中、加载、展开时组件是否不抖动？
6. 页面像调试观察台，而不是营销页？
