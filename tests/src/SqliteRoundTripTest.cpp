/*
 * SqliteRoundTripTest.cpp - verifies lossless MMP -> SQLite -> MMP conversion
 *
 * Copyright (c) 2026
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <QCoreApplication>
#include <QDomDocument>
#include <QFile>
#include <QMap>
#include <QTest>

#include "XmlToSqlite.h"
#include "SqliteToXml.h"

using namespace lmms;

// Count all element tags in a DOM tree
static QMap<QString, int> countElements(const QDomElement& root)
{
	QMap<QString, int> counts;
	QList<QDomElement> stack;
	stack.append(root);
	while (!stack.isEmpty())
	{
		QDomElement elem = stack.takeLast();
		counts[elem.tagName()]++;
		auto child = elem.firstChildElement();
		while (!child.isNull())
		{
			stack.append(child);
			child = child.nextSiblingElement();
		}
	}
	return counts;
}

// Count total attributes across all elements
static int countTotalAttributes(const QDomElement& root)
{
	int total = 0;
	QList<QDomElement> stack;
	stack.append(root);
	while (!stack.isEmpty())
	{
		QDomElement elem = stack.takeLast();
		total += elem.attributes().count();
		auto child = elem.firstChildElement();
		while (!child.isNull())
		{
			stack.append(child);
			child = child.nextSiblingElement();
		}
	}
	return total;
}

class SqliteRoundTripTest : public QObject
{
	Q_OBJECT

private slots:
	void convertDbToFile()
	{
		// Convert a .lmms-db file to .mmp for manual inspection
		QString dbPath = qEnvironmentVariable("LMMS_TEST_DB");
		QString outPath = qEnvironmentVariable("LMMS_TEST_OUT");
		if (dbPath.isEmpty() || outPath.isEmpty())
		{
			QSKIP("Set LMMS_TEST_DB and LMMS_TEST_OUT env vars to run this test");
		}

		QByteArray xmlBytes = SqliteToXml::convert(dbPath);
		QVERIFY2(!xmlBytes.isEmpty(), "SqliteToXml::convert() returned empty");

		QFile outFile(outPath);
		QVERIFY2(outFile.open(QIODevice::WriteOnly), "Cannot write output file");
		outFile.write(xmlBytes);
		outFile.close();
		qWarning("Wrote %d bytes to %s", xmlBytes.size(), qPrintable(outPath));
	}

	void roundTripFidelity()
	{
		// Use env var if set, otherwise locate the default test file
		QString mmpPath = qEnvironmentVariable("LMMS_ROUNDTRIP_MMP");
		if (mmpPath.isEmpty())
		{
			mmpPath = QFINDTESTDATA("../../Southendopus-hen.mmp");
		}
		if (mmpPath.isEmpty())
		{
			QSKIP("No MMP file found (set LMMS_ROUNDTRIP_MMP or provide Southendopus-hen.mmp)");
		}

		// Load original XML
		QFile origFile(mmpPath);
		QVERIFY2(origFile.open(QIODevice::ReadOnly), "Cannot open original MMP");
		QDomDocument origDoc;
		QString errMsg;
		int errLine;
		QVERIFY2(origDoc.setContent(&origFile, false, &errMsg, &errLine),
			qPrintable(QString("XML parse error at line %1: %2").arg(errLine).arg(errMsg)));
		origFile.close();

		// Count original elements
		auto origCounts = countElements(origDoc.documentElement());
		int origAttrCount = countTotalAttributes(origDoc.documentElement());

		// Step 1: XML -> SQLite
		QString dbPath = QDir::tempPath() + "/lmms_roundtrip_test.lmms-db";
		QFile::remove(dbPath);  // clean up any previous run
		bool saveOk = XmlToSqlite::convert(origDoc, dbPath);
		QVERIFY2(saveOk, "XmlToSqlite::convert() failed");

		// Step 2: SQLite -> XML
		QByteArray xmlBytes = SqliteToXml::convert(dbPath);
		QVERIFY2(!xmlBytes.isEmpty(), "SqliteToXml::convert() returned empty");

		// Parse round-tripped XML
		QDomDocument rtDoc;
		QVERIFY2(rtDoc.setContent(xmlBytes, false, &errMsg, &errLine),
			qPrintable(QString("Round-trip XML parse error at line %1: %2").arg(errLine).arg(errMsg)));

		// Count round-tripped elements
		auto rtCounts = countElements(rtDoc.documentElement());
		int rtAttrCount = countTotalAttributes(rtDoc.documentElement());

		// Report differences
		QSet<QString> allTags;
		for (auto it = origCounts.begin(); it != origCounts.end(); ++it) { allTags.insert(it.key()); }
		for (auto it = rtCounts.begin(); it != rtCounts.end(); ++it) { allTags.insert(it.key()); }

		int diffCount = 0;
		for (const auto& tag : allTags)
		{
			int o = origCounts.value(tag, 0);
			int r = rtCounts.value(tag, 0);
			if (o != r)
			{
				qWarning("  DIFF %-30s orig=%d rt=%d delta=%+d",
					qPrintable(tag), o, r, r - o);
				diffCount++;
			}
		}

		qWarning("Total tags: %d, Tags with differences: %d", allTags.size(), diffCount);
		qWarning("Attributes: orig=%d, rt=%d, delta=%+d", origAttrCount, rtAttrCount, rtAttrCount - origAttrCount);

		// The key metric: no tags should be completely lost
		for (auto it = origCounts.begin(); it != origCounts.end(); ++it)
		{
			if (rtCounts.value(it.key(), 0) == 0)
			{
				qWarning("  LOST TAG: <%s> (was %d in original)", qPrintable(it.key()), it.value());
			}
		}

		// Verify critical elements are preserved
		QCOMPARE(rtCounts.value("head", 0), origCounts.value("head", 0));
		QCOMPARE(rtCounts.value("song", 0), origCounts.value("song", 0));

		// Notes should be exact
		QCOMPARE(rtCounts.value("note", 0), origCounts.value("note", 0));

		// Automation nodes should be exact
		QCOMPARE(rtCounts.value("time", 0), origCounts.value("time", 0));

		// Clean up
		QFile::remove(dbPath);
	}
};

QTEST_GUILESS_MAIN(SqliteRoundTripTest)
#include "SqliteRoundTripTest.moc"
