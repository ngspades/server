// The whole manual as one PDF
#import "/template.typ": *
#import "/chapters.typ": chapters
#set document(title: "ngspades-server manual")
#set page(paper: "a4", margin: 2cm)
#align(center + horizon)[
  #text(size: 30pt, weight: "bold", fill: accent)[ngspades-server]
  #v(0.3em)
  #text(size: 20pt)[User manual and Lua plugin reference]
  #v(2em)
  #text(fill: muted)[https:\/\/ngspades.github.io/server/]
]
#pagebreak()
#outline(depth: 2, indent: 1em)
#set heading(numbering: "1.1")
#show: body => paged-page("User manual", body)
#for c in chapters {
  heading(level: 1)[#c.title]
  include "/chapters/" + c.file + ".typ"
}
