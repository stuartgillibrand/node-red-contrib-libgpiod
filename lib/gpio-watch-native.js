const EDGE_MODES = {
  both: 0,
  rising: 1,
  falling: 2,
}

const BIAS_MODES = {
  asis: 0,
  floating: 1,
  pullup: 2,
  pulldown: 3,
}

const UNAVAILABLE_CODE = 'ERR_GPIO_WATCH_NATIVE_UNAVAILABLE'

let nativeBinding = null
let loadError = null

try {
  const loadBindings = require('bindings')
  nativeBinding = loadBindings('libgpiod_watch')
} catch (error) {
  loadError = error
  loadError.code = UNAVAILABLE_CODE
}

function getModeValue(table, value, label) {
  if (Object.prototype.hasOwnProperty.call(table, value)) {
    return table[value]
  }

  throw new Error(`Invalid ${label}: ${value}`)
}

function createWatcher(config, onEvent, onError) {
  if (!nativeBinding) {
    throw loadError
  }

  if (typeof onEvent !== 'function') {
    throw new TypeError('onEvent callback is required')
  }

  return new nativeBinding.Watcher(
    {
      device: config.device,
      pin: config.pin,
      edge: getModeValue(EDGE_MODES, config.edge, 'edge'),
      bias: getModeValue(BIAS_MODES, config.bias, 'bias'),
      consumer: config.consumer || '',
    },
    onEvent,
    typeof onError === 'function' ? onError : function () {}
  )
}

module.exports = {
  createWatcher,
  isAvailable: Boolean(nativeBinding),
  loadError,
  unavailableCode: UNAVAILABLE_CODE,
}