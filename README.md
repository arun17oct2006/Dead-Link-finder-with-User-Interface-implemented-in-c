This project is a UI-based Dead Link Finder developed using the C programming language. It provides a graphical interface to analyze web pages and detect broken or inactive links. GTK is used for building the user interface, libcurl for fetching web pages, and regex for extracting and validating URL patterns. The tool helps identify dead links efficiently, making it useful for website maintenance and link analysis.
## Technologies Used
- C
- GTK
- libcurl
- regex
## Features
- Graphical user interface
- Web page crawling
- URL extraction and validation
- Dead/broken link detection
- ## How it works
- User enters URL
- The page is fetched using libcurl.
- Uses dfs to crawl into links in the given url
- checks for deadlinks if yes it notifies to user
  
