import { Boxes, FlaskConical } from "lucide-react"

import { Button } from "@/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { Separator } from "@/components/ui/separator"
import { ComponentLab } from "@/pages/ComponentLab"

function HomePage() {
  return (
    <main className="min-h-screen bg-background px-6 py-8 text-foreground">
      <div className="mx-auto flex w-full max-w-6xl flex-col gap-8">
        <header className="flex flex-col gap-4">
          <div className="flex items-center gap-3">
            <div className="flex size-10 items-center justify-center rounded-lg border bg-card">
              <Boxes aria-hidden="true" />
            </div>
            <div>
              <p className="text-sm text-muted-foreground">ESC Frontend</p>
              <h1 className="text-3xl font-semibold tracking-normal">MySQL 数字孪生前端工作台</h1>
            </div>
          </div>
          <p className="max-w-3xl text-muted-foreground">
            这里先作为 React 前端入口。组件、动效、图形实验统一放到测试页面，正式业务页面后面再单独展开。
          </p>
        </header>

        <Separator />

        <section className="grid gap-4 md:grid-cols-2">
          <Card>
            <CardHeader>
              <CardTitle>组件实验室</CardTitle>
              <CardDescription>以后想看组件长什么样子，就先放到这个页面。</CardDescription>
            </CardHeader>
            <CardContent>
              <Button asChild>
                <a href="/component-lab">
                  <FlaskConical data-icon="inline-start" aria-hidden="true" />
                  打开测试页面
                </a>
              </Button>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <CardTitle>当前约定</CardTitle>
              <CardDescription>测试页面只负责预览外观，不承载真实业务状态。</CardDescription>
            </CardHeader>
            <CardContent className="text-sm leading-7 text-muted-foreground">
              后面新增按钮、面板、时间线、图节点、详情面板时，可以先在这里摆出来看尺寸、颜色和交互手感。
            </CardContent>
          </Card>
        </section>
      </div>
    </main>
  )
}

function App() {
  const pathname = window.location.pathname

  // 先用轻量路径判断，避免为了一个测试页过早引入完整路由方案。
  if (pathname === "/component-lab") {
    return <ComponentLab />
  }

  return <HomePage />
}

export default App
