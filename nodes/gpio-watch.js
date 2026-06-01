const { spawn } = require('child_process')
const readline = require('readline')

const WATCH_FORMAT = '%e %s %n'

const BIAS_ARGUMENTS = {
  asis: 'as-is',
  pullup: 'pull-up',
  pulldown: 'pull-down',
  floating: 'disable',
}

module.exports = function (RED) {
  function setError(node, msg) {
    node.status({ text: msg, fill: 'red', shape: 'dot' })
    node.error(msg)
  }

  function setWatchingStatus(node) {
    const debounceText = node.debounce > 0 ? ` / ${node.debounce}ms` : ''
    node.status({ text: `watch: ${node.edge}${debounceText}`, fill: 'green', shape: 'dot' })
  }

  function parseEventLine(line) {
    const parts = line.trim().split(/\s+/)

    if (parts.length < 3) {
      return null
    }

    const eventValue = Number(parts[0])
    const seconds = Number(parts[1])
    const nanoseconds = Number(parts[2])

    if (!Number.isFinite(eventValue) || !Number.isFinite(seconds) || !Number.isFinite(nanoseconds)) {
      return null
    }

    return {
      edge: eventValue === 1 ? 'rising' : 'falling',
      payload: eventValue === 1 ? 1 : 0,
      timestamp: {
        seconds,
        nanoseconds,
      },
      timestampMs: seconds * 1000 + nanoseconds / 1e6,
    }
  }

  function buildWatchArgs(node) {
    const args = ['-b', '-F', WATCH_FORMAT, '-B', BIAS_ARGUMENTS[node.bias] || BIAS_ARGUMENTS.asis]

    if (node.edge === 'rising') {
      args.push('-r')
    } else if (node.edge === 'falling') {
      args.push('-f')
    }

    args.push(node.device, String(node.pin))

    return args
  }

  function teardownWatch(node) {
    if (node.watchReader) {
      node.watchReader.removeAllListeners()
      node.watchReader.close()
      node.watchReader = null
    }

    if (node.watchProcess) {
      if (node.watchProcess.stdout) {
        node.watchProcess.stdout.removeAllListeners()
      }

      if (node.watchProcess.stderr) {
        node.watchProcess.stderr.removeAllListeners()
      }

      node.watchProcess.removeAllListeners()
      node.watchProcess = null
    }
  }

  function emitWatchEvent(node, event) {
    if (node.debounce > 0 && node.lastEventTimestampMs !== null) {
      if (event.timestampMs - node.lastEventTimestampMs < node.debounce) {
        return
      }
    }

    node.lastEventTimestampMs = event.timestampMs
    node.status({ text: `${event.edge}: ${event.payload}`, fill: 'green', shape: 'dot' })
    node.send({
      payload: event.payload,
      topic: node.name || `gpio watch #${node.pin}`,
      pin: node.pin,
      device: node.device,
      mode: 'watch',
      edge: event.edge,
      timestamp: event.timestamp,
      timestampMs: event.timestampMs,
      bias: node.bias,
      debounce: node.debounce,
    })
  }

  function startWatch(node) {
    const args = buildWatchArgs(node)
    const child = spawn('gpiomon', args, {
      stdio: ['ignore', 'pipe', 'pipe'],
    })

    node.watchProcess = child
    node.watchStderr = ''

    if (child.stdout) {
      child.stdout.setEncoding('utf8')
      node.watchReader = readline.createInterface({ input: child.stdout })
      node.watchReader.on('line', function (line) {
        const event = parseEventLine(line)

        if (!event) {
          return
        }

        emitWatchEvent(node, event)
      })
    }

    if (child.stderr) {
      child.stderr.setEncoding('utf8')
      child.stderr.on('data', function (chunk) {
        node.watchStderr = `${node.watchStderr}${chunk}`.trim()
      })
    }

    child.once('spawn', function () {
      setWatchingStatus(node)
    })

    child.on('error', function (error) {
      if (node.closing) {
        return
      }

      teardownWatch(node)
      setError(node, `Unable to start gpiomon: ${error.message}`)
    })

    child.on('close', function (code, signal) {
      teardownWatch(node)

      if (node.closing) {
        return
      }

      const stderr = node.watchStderr ? `: ${node.watchStderr}` : ''

      if (code === 0) {
        setError(node, `gpiomon stopped unexpectedly${stderr}`)
        return
      }

      if (code !== null) {
        setError(node, `gpiomon exited with code ${code}${stderr}`)
        return
      }

      setError(node, `gpiomon stopped by signal ${signal}${stderr}`)
    })
  }

  function stopWatch(node, done) {
    const child = node.watchProcess

    node.closing = true

    if (!child) {
      done()
      return
    }

    let finished = false

    function finish() {
      if (finished) {
        return
      }

      finished = true
      teardownWatch(node)
      done()
    }

    child.once('close', finish)

    if (child.exitCode !== null || child.signalCode !== null) {
      finish()
      return
    }

    child.kill('SIGTERM')

    const killTimer = setTimeout(function () {
      if (node.watchProcess === child && child.exitCode === null && child.signalCode === null) {
        child.kill('SIGKILL')
      }

      finish()
    }, 500)

    if (typeof killTimer.unref === 'function') {
      killTimer.unref()
    }
  }

  function GpioWatch(config) {
    RED.nodes.createNode(this, config)
    var node = this
    node.status({})
    node.name = config.name || ''
    node.device = config.device || 'gpiochip0'
    node.pin = Number(config.pin)
    node.bias = config.bias || 'asis'
    node.edge = config.edge || 'both'
    node.debounce = Number(config.debounce)
    node.lastEventTimestampMs = null
    node.watchProcess = null
    node.watchReader = null
    node.watchStderr = ''
    node.closing = false

    if (!Number.isInteger(node.pin) || node.pin < 0) {
      setError(node, 'No pin configured')
      return
    }

    if (!Number.isInteger(node.debounce) || node.debounce < 0) {
      setError(node, 'Debounce must be a non-negative integer')
      return
    }

    if (!BIAS_ARGUMENTS[node.bias]) {
      setError(node, 'Invalid bias configured')
      return
    }

    if (!['both', 'rising', 'falling'].includes(node.edge)) {
      setError(node, 'Invalid edge mode configured')
      return
    }

    startWatch(node)

    node.on('close', function (removed, done) {
      stopWatch(node, done)
    })
  }

  RED.nodes.registerType('gpio-watch', GpioWatch)
}