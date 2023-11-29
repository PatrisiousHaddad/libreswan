<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">

  <!-- Turn citerefentry elements into HTML links -->
  <xsl:param name="citerefentry.link" select="1"/>

  <!-- Code to generate the URL for a given citerefentry element -->
  <xsl:template name="generate.citerefentry.link">
    <xsl:value-of select="refentrytitle"/>
    <xsl:text>.</xsl:text>
    <xsl:value-of select="manvolnum"/>
    <xsl:text>.html</xsl:text>
  </xsl:template>
</xsl:stylesheet>
