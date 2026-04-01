import { $ } from 'bun'
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'

const PORT = process.env.PORT || 3000

const cors = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'POST, OPTIONS',
  'Access-Control-Allow-Headers': 'content-type',
}

Bun.serve({
  port: PORT,
  async fetch(req) {
    if (req.method === 'OPTIONS') return new Response(null, { headers: cors })

    if (req.method === 'POST' && new URL(req.url).pathname === '/generate-log') {
      let url
      try { url = (await req.json()).url } catch {
        return new Response('bad json', { status: 400, headers: cors })
      }

      let parsed
      try { parsed = new URL(url) } catch {
        return new Response('invalid url', { status: 400, headers: cors })
      }
      if (parsed.protocol !== 'https:') {
        return new Response('only https urls allowed', { status: 400, headers: cors })
      }

      const tmp = await mkdtemp(`${tmpdir()}/gloam-`)
      try {
        await $`GIT_TERMINAL_PROMPT=0 git clone --bare --filter=blob:none ${url} repo.git`.cwd(tmp).quiet()

        const log = await $`git log --pretty=format:'%ct|%aN|%H|%s' --name-status --no-renames --reverse`
          .cwd(`${tmp}/repo.git`).text()

        return new Response(log, { headers: { ...cors, 'Content-Type': 'text/plain' } })
      } catch {
        return new Response('clone or log generation failed', { status: 500, headers: cors })
      } finally {
        await rm(tmp, { recursive: true })
      }
    }

    return new Response('not found', { status: 404, headers: cors })
  }
})

console.log(`gloam server on :${PORT}`)
