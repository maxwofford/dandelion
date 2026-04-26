import { $ } from 'bun'
import { mkdtemp, rm, appendFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { resolve, dirname } from 'node:path'

const PORT = process.env.PORT || 3000
const LOG_FILE = process.env.LOG_FILE || 'clones.log'
const EXPORT_TOOL = resolve(dirname(import.meta.path), '../tools/export.js')

const cors = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'content-type',
}

let cloneCount = 0

// count existing lines on startup
try {
  const existing = await Bun.file(LOG_FILE).text()
  cloneCount = existing.trim().split('\n').filter(Boolean).length
} catch {}

Bun.serve({
  port: PORT,
  async fetch(req) {
    const { pathname } = new URL(req.url)

    if (req.method === 'OPTIONS') return new Response(null, { headers: cors })

    if (req.method === 'GET' && pathname === '/stats') {
      return Response.json({ clones: cloneCount }, { headers: cors })
    }

    if (req.method === 'POST' && pathname === '/generate-log') {
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

        cloneCount++
        await appendFile(LOG_FILE, `${new Date().toISOString()} ${url}\n`)

        return new Response(log, { headers: { ...cors, 'Content-Type': 'text/plain' } })
      } catch {
        return new Response('clone or log generation failed', { status: 500, headers: cors })
      } finally {
        await rm(tmp, { recursive: true })
      }
    }

    if (req.method === 'GET' && pathname === '/og-image') {
      const url = new URL(req.url).searchParams.get('url')
      if (!url) return new Response('missing ?url= param', { status: 400, headers: cors })

      let parsed
      try { parsed = new URL(url) } catch {
        return new Response('invalid url', { status: 400, headers: cors })
      }
      if (parsed.protocol !== 'https:') {
        return new Response('only https urls allowed', { status: 400, headers: cors })
      }

      const tmp = await mkdtemp(`${tmpdir()}/gloam-og-`)
      try {
        await $`GIT_TERMINAL_PROMPT=0 git clone --bare --filter=blob:none ${url} repo.git`.cwd(tmp).quiet()

        const logFile = `${tmp}/repo.log`
        const log = await $`git log --pretty=format:'%ct|%aN|%H|%s' --name-status --no-renames --reverse`
          .cwd(`${tmp}/repo.git`).text()
        await Bun.write(logFile, log)

        const outFile = `${tmp}/og.png`
        await $`bun ${EXPORT_TOOL} ${logFile} --format png --width 1200 --height 630 --speed 50 --settle 90 --output ${outFile}`.quiet()

        const png = await Bun.file(outFile).arrayBuffer()
        return new Response(png, {
          headers: { ...cors, 'Content-Type': 'image/png' }
        })
      } catch (e) {
        return new Response('og-image generation failed: ' + e.message, { status: 500, headers: cors })
      } finally {
        await rm(tmp, { recursive: true })
      }
    }

    return new Response('not found', { status: 404, headers: cors })
  }
})

console.log(`gloam server on :${PORT}`)
