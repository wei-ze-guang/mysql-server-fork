import { ArrowLeft, Code2, Eye, MousePointerClick, Play, Search } from "lucide-react"
import { SiMysql, SiPixiv, SiReact, SiThreedotjs, SiVite } from "react-icons/si"

import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Separator } from "@/components/ui/separator"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs"

const previewEvents = [
  "mysql.executor.iterator.open",
  "mysql.handler.api.index_read.call",
  "innodb.btree.search.leaf.enter",
  "innodb.mvcc.visibility.check",
  "mysql.executor.iterator.row.emit",
]

const previewEntities = [
  { id: "query:q1", type: "Query", state: "active" },
  { id: "cursor:q1:idx_user_id", type: "Cursor", state: "positioned" },
  { id: "page:8:5", type: "BTreePage", state: "loaded" },
  { id: "record:8:5:12", type: "Record", state: "visible" },
]

const logoIcons = [
  { label: "React", icon: SiReact },
  { label: "Vite", icon: SiVite },
  { label: "MySQL", icon: SiMysql },
  { label: "PixiJS", icon: SiPixiv },
  { label: "Three.js", icon: SiThreedotjs },
]

export function ComponentLab() {
  return (
    <main className="min-h-screen bg-background px-6 py-8 text-foreground">
      <div className="mx-auto flex w-full max-w-7xl flex-col gap-8">
        <header className="flex flex-col gap-5 md:flex-row md:items-end md:justify-between">
          <div className="flex flex-col gap-3">
            <Button asChild variant="ghost" className="w-fit">
              <a href="/">
                <ArrowLeft data-icon="inline-start" aria-hidden="true" />
                返回首页
              </a>
            </Button>
            <div>
              <p className="text-sm text-muted-foreground">Component Lab</p>
              <h1 className="text-3xl font-semibold tracking-normal">组件测试页面</h1>
            </div>
            <p className="max-w-3xl text-muted-foreground">
              这个页面专门用来临时摆放前端组件，先看清楚外观、间距和交互，再决定要不要放进正式 viewer。
            </p>
          </div>
          <div className="flex flex-wrap gap-2">
            <Badge variant="secondary">shadcn/ui</Badge>
            <Badge variant="outline">React</Badge>
            <Badge variant="outline">Viewer Preview</Badge>
          </div>
        </header>

        <Separator />

        <section className="grid gap-4 lg:grid-cols-[1.1fr_0.9fr]">
          <Card>
            <CardHeader>
              <CardTitle>按钮和操作状态</CardTitle>
              <CardDescription>先把常用操作摆出来，后面做工具栏时可以直接对比。</CardDescription>
            </CardHeader>
            <CardContent className="flex flex-wrap gap-3">
              <Button>
                <Play data-icon="inline-start" aria-hidden="true" />
                回放
              </Button>
              <Button variant="secondary">
                <Eye data-icon="inline-start" aria-hidden="true" />
                聚焦实体
              </Button>
              <Button variant="outline">
                <Search data-icon="inline-start" aria-hidden="true" />
                查找
              </Button>
              <Button variant="ghost">
                <MousePointerClick data-icon="inline-start" aria-hidden="true" />
                选择
              </Button>
              <Button variant="destructive">错误状态</Button>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <CardTitle>状态标签</CardTitle>
              <CardDescription>用于事件、实体、组件状态的轻量提示。</CardDescription>
            </CardHeader>
            <CardContent className="flex flex-wrap gap-2">
              <Badge>active</Badge>
              <Badge variant="secondary">visible</Badge>
              <Badge variant="outline">unknown</Badge>
              <Badge variant="destructive">error</Badge>
            </CardContent>
          </Card>
        </section>

        <Card>
          <CardHeader>
            <CardTitle>React Icons Logo 预览</CardTitle>
            <CardDescription>以后想看某个技术 logo 或小图标效果，可以先放到这里试尺寸和搭配。</CardDescription>
          </CardHeader>
          <CardContent className="grid gap-3 sm:grid-cols-2 lg:grid-cols-5">
            {logoIcons.map(({ label, icon: Icon }) => (
              <div className="flex items-center gap-3 rounded-lg border bg-card p-4" key={label}>
                <Icon aria-hidden="true" className="size-6 text-foreground" />
                <span className="text-sm font-medium">{label}</span>
              </div>
            ))}
          </CardContent>
        </Card>

        <Tabs defaultValue="timeline" className="gap-4">
          <TabsList>
            <TabsTrigger value="timeline">时间线</TabsTrigger>
            <TabsTrigger value="entities">实体列表</TabsTrigger>
            <TabsTrigger value="detail">详情面板</TabsTrigger>
          </TabsList>

          <TabsContent value="timeline">
            <Card>
              <CardHeader>
                <CardTitle>Timeline 预览</CardTitle>
                <CardDescription>以后可以把真实 scene events 的展示样式放到这里试。</CardDescription>
              </CardHeader>
              <CardContent>
                <ScrollArea className="h-64 rounded-md border">
                  <div className="flex flex-col gap-3 p-4">
                    {previewEvents.map((eventName, index) => (
                      <div
                        className="grid gap-3 rounded-lg border bg-card p-3 text-sm md:grid-cols-[72px_1fr_auto]"
                        key={eventName}
                      >
                        <span className="font-mono text-muted-foreground">#{index + 1}</span>
                        <span className="font-mono">{eventName}</span>
                        <Badge variant="outline">tick {index + 1}</Badge>
                      </div>
                    ))}
                  </div>
                </ScrollArea>
              </CardContent>
            </Card>
          </TabsContent>

          <TabsContent value="entities">
            <section className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
              {previewEntities.map((entity) => (
                <Card key={entity.id}>
                  <CardHeader>
                    <CardTitle className="text-base">{entity.type}</CardTitle>
                    <CardDescription className="font-mono">{entity.id}</CardDescription>
                  </CardHeader>
                  <CardContent>
                    <Badge variant={entity.state === "visible" ? "secondary" : "outline"}>
                      {entity.state}
                    </Badge>
                  </CardContent>
                </Card>
              ))}
            </section>
          </TabsContent>

          <TabsContent value="detail">
            <Card>
              <CardHeader>
                <CardTitle>Entity Detail 预览</CardTitle>
                <CardDescription>用于观察字段排版和 JSON 信息的可读性。</CardDescription>
              </CardHeader>
              <CardContent>
                <div className="rounded-lg border bg-muted/40 p-4">
                  <div className="mb-3 flex items-center gap-2 text-sm font-medium">
                    <Code2 aria-hidden="true" />
                    record:8:5:12
                  </div>
                  <pre className="overflow-auto rounded-md bg-background p-4 text-sm leading-7">
{`{
  "entity_type": "Record",
  "page_no": 5,
  "heap_no": 12,
  "visible_state": "visible"
}`}
                  </pre>
                </div>
              </CardContent>
            </Card>
          </TabsContent>
        </Tabs>
      </div>
    </main>
  )
}
