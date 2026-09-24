# 部署与落地

`literouter` 是单用户网关：一把 `server.api_key` 同时守护模型接口与管理接口。若要通过监听侧 TLS 或可信反向代理在本机之外访问，见[配置与密钥](configuration.md)中的 TLS 一节，并设置该密钥。

## 落地方式

仓库提供四种落地方式，选择哪一种取决于这台机器上有什么：

| 方式 | 文件 | 适用 |
|---|---|---|
| 直接跑二进制 | `mcpp build -p cli --release` | 从源码构建后直接运行；适合个人本地使用 |
| systemd 服务 | `deploy/literouter.service` | 用发行版自带的服务管理；独立账户、`ProtectSystem=strict`、`SIGTERM` 优雅停机 |
| Docker | `Dockerfile` | 容器化主机。多阶段构建，运行镜像只装 `ca-certificates` 与一个二进制，以 uid 8787 非 root 运行 |
| Docker Compose | `docker-compose.yml` | 需要把配置与状态分开持久化的场景 |

仓库不提供公开 Release、预编译下载包或一键下载安装脚本。需要直接运行时，请在本机从源码构建 CLI，然后使用生成的 `literouter`；长期运行可配合 systemd，或使用 Docker / Compose。

```bash
mcpp build -p core --release
mcpp build -p cli --release
./cli/target/*/*/bin/literouter config init
./cli/target/*/*/bin/literouter serve
```

**Windows 上的配置路径是一条普通路径，没有前缀。** 构建用的是 MinGW GCC，从不把路径写成 `\\?\` 长路径形式，也没有随包提供 `longPathAware` 清单，因此路径仍受 Windows 对普通路径施加的 `MAX_PATH` 规则约束。把配置留在默认位置 `%APPDATA%\literouter\`（路径天然很短）就永远不会碰到这个问题。万一系统确实拒绝了某个路径，失败是响亮而无害的，不会静默：保存会报 `cannot write config …` 并以非零码退出，且不会动到原文件——新内容先写到临时名，落盘之后才会被改名覆盖旧文件。

两条与运维有关的注意事项：

- **配置与状态必须分开挂载。** 配置（`/etc/literouter`）里有密钥，是你编辑和备份的对象；状态目录（`/var/lib/literouter`）里是遥测文件。把状态目录放在匿名卷上，等于每次 `docker compose down -v` 都清空一次历史遥测。
- **容器里的监听地址是 `0.0.0.0`，端口映射请绑到宿主机回环**（`-p 127.0.0.1:8787:8787`）。`/ui` 控制台能读请求日志，而请求日志可能包含提示词；把它暴露到 0.0.0.0 且不设 `server.api_key` 时，校验器会给出警告，但那应该是一个刻意的决定，而不是从 compose 文件里继承来的默认值。

**改端口时改配置文件，不要改 `docker run`。** 容器的 `CMD` 只钉住了 `--host 0.0.0.0`（否则会监听容器内的回环，映射出去的端口没人应答），端口留给配置文件的 `server.port`。也就是说容器里的 `server.port` 和本机运行时含义一致：

```bash
# 配置写 server.port = 9000，就发布 9000
docker run -p 127.0.0.1:9000:9000 …
```

`EXPOSE 8787` 只是文档性声明，不决定实际监听端口。健康检查 `literouter status` 读的是同一份配置，因此会自动跟着 `server.port` 走，不需要同步修改。

**首次启动时配置卷是空的**，此时容器会打印一条警告（`config … does not exist; running with the built-in seed`）并使用内置 seed 配置启动，而不是崩溃。内置 seed 指向示例中转站，因此在写入真实配置前不要对外发布端口：

```bash
docker compose up -d
docker compose exec literouter literouter config init --force   # 写入 seed 文件
docker compose restart
```

健康检查用 `literouter status --json --quiet`：它走的是与客户端同一条管理 API 路径，因此检查的是「监听器 + 配置 + 管理面」这一整条链路，而不只是「进程还在不在」。就绪与存活的区别见[协议与 API](protocols-api.md)。
