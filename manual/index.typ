// The landing page (HTML only)
#import "/template.typ": *
#import "/chapters.typ": chapters
#chapter-page("index", "ngspades-server manual")[
  The user manual and the Lua plugin reference of *ngspades-server*, the game server of ngspades: install it, configure it, run it for a community, and write plugins and game modes for it.

  #html.elem("div", attrs: (class: "cards"))[
    #for c in chapters {
      html.elem("a", attrs: (class: "card", href: c.file + ".html"))[
        #html.elem("b")[#c.part]
        #c.title
      ]
    }
  ]

  #html.elem("div", attrs: (class: "pdfs"))[
    Offline: #link("ngspades-server-manual.pdf")[the full manual as one PDF]. Source: #link("https://github.com/ngspades/server")[github.com/ngspades/server], in the `manual/` folder. Plugins: #link("https://github.com/ngspades/plugins")[github.com/ngspades/plugins].
  ]
]
