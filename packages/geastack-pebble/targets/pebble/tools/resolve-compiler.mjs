import { existsSync, readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import { dirname, resolve } from 'node:path'

const [target, core] = process.argv.slice(2)
if (!target || !core) throw new Error('usage: resolve-compiler.mjs <pebble-target-dir> <core-package-dir>')
const packageName = (directory) => {
  const manifest = resolve(directory, 'package.json')
  return existsSync(manifest) ? JSON.parse(readFileSync(manifest, 'utf8')).name : undefined
}

let compiler = process.env.GEA_GEATSC_BIN
if (!compiler) {
  const repository = resolve(target, '../../../..')
  const sibling = resolve(repository, '../compiler')
  // Only a development checkout opts into the sibling compiler. An npm
  // installation continues to use the compiler installed with its core.
  if (packageName(repository) === 'geastack-pebble-repo' && packageName(sibling) === '@geastack/compiler') {
    compiler = resolve(sibling, 'dist/cli.js')
    if (!existsSync(compiler)) throw new Error(`Build the workspace compiler first: cd ${sibling} && npm run build`)
  } else {
    const require = createRequire(resolve(core, 'package.json'))
    compiler = resolve(dirname(require.resolve('@geastack/compiler/package.json')), 'dist/cli.js')
  }
}
compiler = resolve(compiler)
if (!existsSync(compiler)) throw new Error(`Pebble compiler does not exist: ${compiler}`)
console.log(compiler)
