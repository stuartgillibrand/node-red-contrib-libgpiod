{
  "targets": [
    {
      "target_name": "libgpiod_watch",
      "sources": [
        "src/gpio_watch_addon.cc"
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")"
      ],
      "libraries": [
        "-lgpiod"
      ]
    }
  ]
}