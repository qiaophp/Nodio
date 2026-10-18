import urllib.request
def getHtml(url): 
    b = urllib.request.urlopen(url).read()
    return b.decode("utf-8")